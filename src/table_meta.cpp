#include "table_meta.h"

#include <format>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <utility>

#include "crc32_helpers.h"
#include "endian_io.h"

using namespace SSTableEntities;

namespace {

    constexpr std::uint64_t TABLE_META_FIXED_DISK_BYTES =
        sizeof(std::uint64_t) * 8ull + // table_id + 7 remaining uint64 fields
        sizeof(std::uint32_t) * 4ull;  // level + three variable-length sizes

    static_assert(TABLE_META_FIXED_DISK_BYTES == 80);

    Result<std::uint32_t> checked_u32_size(std::uint64_t value, const char* name)
    {
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            return Result<std::uint32_t>::fail(
                Status{
                    StatusCode::AllocationTooLarge,
                    std::format("{} size {} exceeds uint32_t", name, value)
                }
            );
        }

        return Result<std::uint32_t>::ok(static_cast<std::uint32_t>(value));
    }

    std::uint64_t variable_payload_size(
        std::uint32_t path_size,
        std::uint32_t smallest_key_size,
        std::uint32_t largest_key_size
    ) noexcept
    {
        return static_cast<std::uint64_t>(path_size) +
            static_cast<std::uint64_t>(smallest_key_size) +
            static_cast<std::uint64_t>(largest_key_size);
    }

    Status validate_variable_payload_size(
        std::uint32_t path_size,
        std::uint32_t smallest_key_size,
        std::uint32_t largest_key_size
    )
    {
        const std::uint64_t bytes = variable_payload_size(
            path_size,
            smallest_key_size,
            largest_key_size
        );

        if (bytes > MAX_TABLE_META_VARIABLE_BYTES) {
            return Status{
                StatusCode::AllocationTooLarge,
                std::format(
                    "table meta variable payload {} bytes exceeds maximum {}",
                    bytes,
                    MAX_TABLE_META_VARIABLE_BYTES
                )
            };
        }

        return Status::ok();
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
            return Status{
                StatusCode::NullPointer,
                "cannot write non-empty byte range from null pointer"
            };
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
            return Status{
                StatusCode::AllocationFailed,
                "arena returned null for non-empty allocation"
            };
        }

        return kvdb::blockio::read_bytes(
            file,
            reinterpret_cast<std::byte*>(out),
            size,
            offset,
            MANIFEST_BLOCK_SIZE
        );
    }

} // namespace

Status TableMeta::validate() const
{
    const std::string path_bytes = path.generic_string();

    auto path_size_result = checked_u32_size(path_bytes.size(), "path");
    if (!path_size_result.is_ok()) {
        return std::move(path_size_result.status);
    }

    if (smallest_key.size != 0 && smallest_key.data == nullptr) {
        return Status{
            StatusCode::NullPointer,
            "smallest key has non-zero size but null data"
        };
    }

    if (largest_key.size != 0 && largest_key.data == nullptr) {
        return Status{
            StatusCode::NullPointer,
            "largest key has non-zero size but null data"
        };
    }

    Status payload_size_status = validate_variable_payload_size(
        path_size_result.value,
        smallest_key.size,
        largest_key.size
    );
    if (!payload_size_status.is_ok()) {
        return payload_size_status;
    }

    if (min_seq > max_seq) {
        return Status{
            StatusCode::Corruption,
            std::format(
                "table min sequence {} exceeds max sequence {}",
                min_seq,
                max_seq
            )
        };
    }

    if (tombstone_count > record_count) {
        return Status{
            StatusCode::Corruption,
            std::format(
                "table tombstone count {} exceeds record count {}",
                tombstone_count,
                record_count
            )
        };
    }

    if (data_bytes > file_size) {
        return Status{
            StatusCode::Corruption,
            std::format(
                "table data bytes {} exceeds file size {}",
                data_bytes,
                file_size
            )
        };
    }

    // Empty byte strings are allowed as keys. Lexicographically, an empty key
    // is smaller than any non-empty key.
    if (record_count != 0) {
        if (smallest_key.size != 0 && largest_key.size == 0) {
            return Status{
                StatusCode::Corruption,
                "table smallest key is greater than empty largest key"
            };
        }

        if (smallest_key.size != 0 &&
            largest_key.size != 0 &&
            largest_key < smallest_key) {
            return Status{
                StatusCode::Corruption,
                "table largest key is smaller than smallest key"
            };
        }
    }

    return Status::ok();
}

Result<std::uint32_t> TableMeta::disk_size() const
{
    Status validation = validate();
    if (!validation.is_ok()) {
        return Result<std::uint32_t>::fail(std::move(validation));
    }

    const std::string path_bytes = path.generic_string();
    auto path_size_result = checked_u32_size(path_bytes.size(), "path");
    if (!path_size_result.is_ok()) {
        return Result<std::uint32_t>::fail(std::move(path_size_result.status));
    }

    const std::uint64_t size =
        TABLE_META_FIXED_DISK_BYTES +
        variable_payload_size(
            path_size_result.value,
            smallest_key.size,
            largest_key.size
        );

    return checked_u32_size(size, "serialized table meta");
}

Status TableMeta::calculate_crc(std::uint32_t& crc_buffer, bool init) const
{
    Status validation = validate();
    if (!validation.is_ok()) {
        return validation;
    }

    if (init) {
        crc_buffer = ::crc32(0, Z_NULL, 0);
    }

    const std::string path_bytes = path.generic_string();
    const auto path_size_result = checked_u32_size(path_bytes.size(), "path");
    if (!path_size_result.is_ok()) {
        return path_size_result.status;
    }

    const std::uint32_t path_size = path_size_result.value;

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

    if (path_size != 0) {
        compute_crc32(crc_buffer, path_bytes.data(), path_size);
    }
    if (smallest_key.size != 0) {
        compute_crc32(crc_buffer, smallest_key.data, smallest_key.size);
    }
    if (largest_key.size != 0) {
        compute_crc32(crc_buffer, largest_key.data, largest_key.size);
    }

    return Status::ok();
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

    meta.record_count = meta_section.payload.record_count;
    meta.tombstone_count = meta_section.payload.tombstone_count;
    meta.min_seq = meta_section.payload.min_seq_num;
    meta.max_seq = meta_section.payload.max_seq_num;
    meta.data_block_count = meta_section.payload.data_block_count;
    meta.data_bytes = meta_section.payload.data_bytes;
    meta.file_size = file_footer_section.file_size;

    if (meta_section.payload.min_key_size != 0 &&
        meta_section.payload.min_key_ptr == nullptr) {
        return Result<TableMeta>::fail(
            Status{
                StatusCode::NullPointer,
                "SSTable min key has non-zero size but null data"
            }
        );
    }

    if (meta_section.payload.max_key_size != 0 &&
        meta_section.payload.max_key_ptr == nullptr) {
        return Result<TableMeta>::fail(
            Status{
                StatusCode::NullPointer,
                "SSTable max key has non-zero size but null data"
            }
        );
    }

    const Arena::Checkpoint checkpoint = arena.checkpoint();

    auto fail_and_rollback = [&](Status status) -> Result<TableMeta> {
        arena.rollback(checkpoint);
        return Result<TableMeta>::fail(std::move(status));
        };

    auto smallest = ArenaEntry::make_entry(
        arena,
        std::span<const std::byte>(
            static_cast<const std::byte*>(meta_section.payload.min_key_ptr),
            meta_section.payload.min_key_size
        )
    );
    if (!smallest.is_ok()) {
        return fail_and_rollback(std::move(smallest.status));
    }

    auto largest = ArenaEntry::make_entry(
        arena,
        std::span<const std::byte>(
            static_cast<const std::byte*>(meta_section.payload.max_key_ptr),
            meta_section.payload.max_key_size
        )
    );
    if (!largest.is_ok()) {
        return fail_and_rollback(std::move(largest.status));
    }

    meta.smallest_key = std::move(smallest.value);
    meta.largest_key = std::move(largest.value);

    Status validation = meta.validate();
    if (!validation.is_ok()) {
        return fail_and_rollback(std::move(validation));
    }

    return Result<TableMeta>::ok(std::move(meta));
}

Status TableMeta::write(WritableFile& file, std::uint64_t& offset) const
{
    // The full object is checked before touching the file. This prevents an
    // invalid in-memory TableMeta from creating a partial manifest record.
    Status validation = validate();
    if (!validation.is_ok()) {
        return validation;
    }

    const std::string path_bytes = path.generic_string();
    auto path_size_result = checked_u32_size(path_bytes.size(), "path");
    if (!path_size_result.is_ok()) {
        return std::move(path_size_result.status);
    }
    const std::uint32_t path_size = path_size_result.value;

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

Result<TableMeta> TableMeta::load(
    ReadableFile& file,
    std::uint64_t& offset,
    Arena& arena
)
{
    // Never publish a partially advanced offset.
    std::uint64_t cursor = offset;

    // Any key allocations made while decoding this record are discarded on
    // failure, including failures after the first key has already been read.
    const Arena::Checkpoint checkpoint = arena.checkpoint();

    auto fail_and_rollback = [&](Status status) -> Result<TableMeta> {
        arena.rollback(checkpoint);
        return Result<TableMeta>::fail(std::move(status));
        };

    Status read_result;
    TableMeta result{};

    read_result = kvdb::blockio::read_u64_t_le(file, result.table_id, cursor, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return fail_and_rollback(std::move(read_result));

    read_result = kvdb::blockio::read_u32_t_le(file, result.level, cursor, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return fail_and_rollback(std::move(read_result));

    read_result = kvdb::blockio::read_u64_t_le(file, result.min_seq, cursor, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return fail_and_rollback(std::move(read_result));

    read_result = kvdb::blockio::read_u64_t_le(file, result.max_seq, cursor, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return fail_and_rollback(std::move(read_result));

    read_result = kvdb::blockio::read_u64_t_le(file, result.file_size, cursor, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return fail_and_rollback(std::move(read_result));

    read_result = kvdb::blockio::read_u64_t_le(file, result.record_count, cursor, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return fail_and_rollback(std::move(read_result));

    read_result = kvdb::blockio::read_u64_t_le(file, result.tombstone_count, cursor, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return fail_and_rollback(std::move(read_result));

    read_result = kvdb::blockio::read_u64_t_le(file, result.data_block_count, cursor, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return fail_and_rollback(std::move(read_result));

    read_result = kvdb::blockio::read_u64_t_le(file, result.data_bytes, cursor, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return fail_and_rollback(std::move(read_result));

    std::uint32_t path_size = 0;

    read_result = kvdb::blockio::read_u32_t_le(file, path_size, cursor, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return fail_and_rollback(std::move(read_result));

    read_result = kvdb::blockio::read_u32_t_le(file, result.smallest_key.size, cursor, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return fail_and_rollback(std::move(read_result));

    read_result = kvdb::blockio::read_u32_t_le(file, result.largest_key.size, cursor, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return fail_and_rollback(std::move(read_result));

    Status payload_size_status = validate_variable_payload_size(
        path_size,
        result.smallest_key.size,
        result.largest_key.size
    );
    if (!payload_size_status.is_ok()) {
        return fail_and_rollback(std::move(payload_size_status));
    }

    std::string path_buffer;
    try {
        path_buffer.assign(path_size, '\0');
    }
    catch (const std::bad_alloc&) {
        return fail_and_rollback(
            Status{
                StatusCode::AllocationFailed,
                "failed to allocate table meta path buffer"
            }
        );
    }

    if (path_size != 0) {
        read_result = kvdb::blockio::read_bytes(
            file,
            reinterpret_cast<std::byte*>(path_buffer.data()),
            path_size,
            cursor,
            MANIFEST_BLOCK_SIZE
        );
        if (!read_result.is_ok()) {
            return fail_and_rollback(std::move(read_result));
        }
    }

    // Construct the path before allocating keys in the Arena so a path
    // conversion/allocation exception cannot strand Arena allocations.
    try {
        result.path = std::filesystem::path(path_buffer);
    }
    catch (const std::bad_alloc&) {
        return fail_and_rollback(
            Status{
                StatusCode::AllocationFailed,
                "failed to allocate table meta filesystem path"
            }
        );
    }
    catch (const std::filesystem::filesystem_error& e) {
        return fail_and_rollback(
            Status{
                StatusCode::Corruption,
                std::format("invalid serialized table path: {}", e.what())
            }
        );
    }

    read_result = read_arena_bytes(
        file,
        result.smallest_key.size,
        cursor,
        arena,
        result.smallest_key.data
    );
    if (!read_result.is_ok()) {
        return fail_and_rollback(std::move(read_result));
    }

    read_result = read_arena_bytes(
        file,
        result.largest_key.size,
        cursor,
        arena,
        result.largest_key.data
    );
    if (!read_result.is_ok()) {
        return fail_and_rollback(std::move(read_result));
    }

    Status validation = result.validate();
    if (!validation.is_ok()) {
        return fail_and_rollback(std::move(validation));
    }

    // Commit only after the full record has been decoded and validated.
    offset = cursor;
    return Result<TableMeta>::ok(std::move(result));
}