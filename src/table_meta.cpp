#include "table_meta.h"
#include <format>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include "crc32_helpers.h"
#include "endian_io.h"

using namespace SSTableEntities;

namespace {

    Result<std::uint32_t> checked_u32_size(std::uint64_t value, const char* name)
    {
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            return Result<std::uint32_t>::fail(
                Status{ StatusCode::AllocationTooLarge, std::format("{} size {} exceeds uint32_t", name, value) }
            );
        }

        return Result<std::uint32_t>::ok(static_cast<std::uint32_t>(value));
    }

    Status write_raw_bytes(
        WritableFile& file,
        const void* data,
        std::uint32_t size,
        std::uint64_t& offset
    )
    {
        if (size == 0) {
            return Status::ok();
        }

        if (data == nullptr) {
            return Status{ StatusCode::NullPointer, "cannot write non-empty byte range from null pointer" };
        }

        return kvdb::blockio::write_bytes(
            file,
            std::span<std::byte>(
                reinterpret_cast<std::byte*>(const_cast<void*>(data)),
                size
            ),
            offset,
            MANIFEST_BLOCK_SIZE
        );
    }

    Status read_arena_bytes(
        ReadableFile& file,
        std::uint32_t size,
        std::uint64_t& offset,
        Arena& arena,
        void*& out
    )
    {
        out = nullptr;

        if (size == 0) {
            return Status::ok();
        }

        Result<void*> alloc_result = arena.alloc(size, alignof(std::byte));
        if (!alloc_result.is_ok()) {
            return std::move(alloc_result.status);
        }

        out = alloc_result.value;
        if (out == nullptr) {
            return Status{ StatusCode::AllocationFailed, "arena returned null for non-empty allocation" };
        }

        Status read_result = kvdb::blockio::read_bytes(
            file,
            reinterpret_cast<std::byte*>(out),
            size,
            offset,
            MANIFEST_BLOCK_SIZE
        );

        if (!read_result.is_ok()) {
            return read_result;
        }

        return Status::ok();
    }

} // namespace

std::uint32_t TableMeta::disk_size() const
{
    const std::string path_bytes = path.generic_string();

    const std::uint64_t size =
        sizeof(table_id) +
        sizeof(level) +
        sizeof(min_seq) +
        sizeof(max_seq) +
        sizeof(file_size) +
        sizeof(record_count) +
        sizeof(tombstone_count) +
        sizeof(data_block_count) +
        sizeof(data_bytes) +
        sizeof(std::uint32_t) + // path size
        sizeof(std::uint32_t) + // smallest key size
        sizeof(std::uint32_t) + // largest key size
        static_cast<std::uint64_t>(path_bytes.size()) +
        smallest_key.size +
        largest_key.size;

    return static_cast<std::uint32_t>(size);
}

void TableMeta::calculate_crc(std::uint32_t& crc_buffer, bool init) const
{
    if (init) {
        crc_buffer = ::crc32(0, Z_NULL, 0);
    }

    const std::string path_bytes = path.generic_string();
    const std::uint32_t path_size = static_cast<std::uint32_t>(path_bytes.size());

    crc32_add_pod<std::uint64_t>(crc_buffer, table_id);
    crc32_add_pod<std::uint32_t>(crc_buffer, level);
    crc32_add_pod<std::uint64_t>(crc_buffer, min_seq);
    crc32_add_pod<std::uint64_t>(crc_buffer, max_seq);
    crc32_add_pod<std::uint64_t>(crc_buffer, file_size);
    crc32_add_pod<std::uint64_t>(crc_buffer, record_count);
    crc32_add_pod<std::uint64_t>(crc_buffer, tombstone_count);
    crc32_add_pod<std::uint64_t>(crc_buffer, data_block_count);
    crc32_add_pod<std::uint64_t>(crc_buffer, data_bytes);

    crc32_add_pod<std::uint32_t>(crc_buffer, path_size);
    crc32_add_pod<std::uint32_t>(crc_buffer, smallest_key.size);
    crc32_add_pod<std::uint32_t>(crc_buffer, largest_key.size);

    compute_crc32(crc_buffer, path_bytes.data(), path_size);
    compute_crc32(crc_buffer, smallest_key.data, smallest_key.size);
    compute_crc32(crc_buffer, largest_key.data, largest_key.size);
}

Result<TableMeta> make_table_meta(
    const SSTable& sstable,
    std::uint32_t level,
    Arena& arena
)
{
    TableMeta meta{};
    const FileHeaderSection& file_header_section = sstable.get_file_header_section();
    const MetaSection& meta_section = sstable.get_meta_section();
    const FileFooterSection& file_footer_section = sstable.get_file_footer_section();

    meta.table_id = file_header_section.table_id;
    meta.level = level;
    meta.path = sstable.get_path();

    auto smallest = ArenaEntry::make_entry(
        arena,
        std::span<const std::byte>(
            static_cast<const std::byte*>(meta_section.payload.min_key_ptr),
            meta_section.payload.min_key_size
        )
    );
    if (!smallest.is_ok()) {
        return Result<TableMeta>::fail(std::move(smallest.status));
    }

    auto largest = ArenaEntry::make_entry(
        arena,
        std::span<const std::byte>(
            static_cast<const std::byte*>(meta_section.payload.max_key_ptr),
            meta_section.payload.max_key_size
        )
    );
    if (!largest.is_ok()) {
        return Result<TableMeta>::fail(std::move(largest.status));
    }

    meta.smallest_key = std::move(smallest.value);
    meta.largest_key = std::move(largest.value);

    meta.record_count = meta_section.payload.record_count;
    meta.tombstone_count = meta_section.payload.tombstone_count;
    meta.min_seq = meta_section.payload.min_seq_num;
    meta.max_seq = meta_section.payload.max_seq_num;
    meta.data_block_count = meta_section.payload.data_block_count;
    meta.data_bytes = meta_section.payload.data_bytes;
    meta.file_size = file_footer_section.file_size;

    return Result<TableMeta>::ok(std::move(meta));
}

Status TableMeta::write(WritableFile& file, std::uint64_t& offset) const
{
    Result<std::uint64_t> current_position = file.current_position();
    if (!current_position.is_ok()) {
        return std::move(current_position.status);
    }

    if (current_position.value != offset) {
        return Status{
            StatusCode::InvalidOffset,
            std::format(
                "writable file current offset {} and tracked offset {} differ",
                current_position.value,
                offset
            )
        };
    }

    const std::string path_bytes = path.generic_string();
    auto path_size_result = checked_u32_size(path_bytes.size(), "path");
    if (!path_size_result.is_ok()) {
        return std::move(path_size_result.status);
    }

    const std::uint32_t path_size = path_size_result.value;

    Status result;

    result = kvdb::blockio::write_u64_t_le(file, table_id, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) return result;

    result = kvdb::blockio::write_u32_t_le(file, level, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) return result;

    result = kvdb::blockio::write_u64_t_le(file, min_seq, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) return result;

    result = kvdb::blockio::write_u64_t_le(file, max_seq, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) return result;

    result = kvdb::blockio::write_u64_t_le(file, file_size, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) return result;

    result = kvdb::blockio::write_u64_t_le(file, record_count, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) return result;

    result = kvdb::blockio::write_u64_t_le(file, tombstone_count, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) return result;

    result = kvdb::blockio::write_u64_t_le(file, data_block_count, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) return result;

    result = kvdb::blockio::write_u64_t_le(file, data_bytes, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) return result;

    result = kvdb::blockio::write_u32_t_le(file, path_size, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) return result;

    result = kvdb::blockio::write_u32_t_le(file, smallest_key.size, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) return result;

    result = kvdb::blockio::write_u32_t_le(file, largest_key.size, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) return result;

    result = write_raw_bytes(file, path_bytes.data(), path_size, offset);
    if (!result.is_ok()) return result;

    result = write_raw_bytes(file, smallest_key.data, smallest_key.size, offset);
    if (!result.is_ok()) return result;

    result = write_raw_bytes(file, largest_key.data, largest_key.size, offset);
    if (!result.is_ok()) return result;

    return Status::ok();
}

Result<TableMeta> TableMeta::load(ReadableFile& file, std::uint64_t& offset, Arena& arena)
{
    Status read_result;
    TableMeta result{};

    read_result = kvdb::blockio::read_u64_t_le(file, result.table_id, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return Result<TableMeta>::fail(std::move(read_result));

    read_result = kvdb::blockio::read_u32_t_le(file, result.level, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return Result<TableMeta>::fail(std::move(read_result));

    read_result = kvdb::blockio::read_u64_t_le(file, result.min_seq, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return Result<TableMeta>::fail(std::move(read_result));

    read_result = kvdb::blockio::read_u64_t_le(file, result.max_seq, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return Result<TableMeta>::fail(std::move(read_result));

    read_result = kvdb::blockio::read_u64_t_le(file, result.file_size, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return Result<TableMeta>::fail(std::move(read_result));

    read_result = kvdb::blockio::read_u64_t_le(file, result.record_count, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return Result<TableMeta>::fail(std::move(read_result));

    read_result = kvdb::blockio::read_u64_t_le(file, result.tombstone_count, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return Result<TableMeta>::fail(std::move(read_result));

    read_result = kvdb::blockio::read_u64_t_le(file, result.data_block_count, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return Result<TableMeta>::fail(std::move(read_result));

    read_result = kvdb::blockio::read_u64_t_le(file, result.data_bytes, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return Result<TableMeta>::fail(std::move(read_result));

    std::uint32_t path_size = 0;

    read_result = kvdb::blockio::read_u32_t_le(file, path_size, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return Result<TableMeta>::fail(std::move(read_result));

    read_result = kvdb::blockio::read_u32_t_le(file, result.smallest_key.size, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return Result<TableMeta>::fail(std::move(read_result));

    read_result = kvdb::blockio::read_u32_t_le(file, result.largest_key.size, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return Result<TableMeta>::fail(std::move(read_result));

    const std::uint64_t variable_bytes =
        static_cast<std::uint64_t>(path_size) +
        result.smallest_key.size +
        result.largest_key.size;

    if (variable_bytes > MANIFEST_BLOCK_SIZE * 16ull) {
        return Result<TableMeta>::fail(
            Status{
                StatusCode::AllocationTooLarge,
                std::format("table meta variable payload {} bytes is suspiciously large", variable_bytes)
            }
        );
    }

    std::string path_buffer(path_size, '\0');
    if (path_size != 0) {
        read_result = kvdb::blockio::read_bytes(
            file,
            reinterpret_cast<std::byte*>(path_buffer.data()),
            path_size,
            offset,
            MANIFEST_BLOCK_SIZE
        );
        if (!read_result.is_ok()) {
            return Result<TableMeta>::fail(std::move(read_result));
        }
    }

    result.path = std::filesystem::path(path_buffer);

    read_result = read_arena_bytes(
        file,
        result.smallest_key.size,
        offset,
        arena,
        result.smallest_key.data
    );
    if (!read_result.is_ok()) return Result<TableMeta>::fail(std::move(read_result));

    read_result = read_arena_bytes(
        file,
        result.largest_key.size,
        offset,
        arena,
        result.largest_key.data
    );
    if (!read_result.is_ok()) return Result<TableMeta>::fail(std::move(read_result));

    return Result<TableMeta>::ok(std::move(result));
}
