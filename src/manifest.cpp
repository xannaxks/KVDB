#include "manifest.h"
#include "crc32_helpers.h"
#include "endian_io.h"
#include <algorithm>
#include <filesystem>
#include <format>
#include <limits>
#include <utility>

namespace {

    constexpr std::uint32_t VERSION_EDIT_HAS_NEXT_TABLE_ID = 1u << 0;
    constexpr std::uint32_t VERSION_EDIT_HAS_NEXT_SEQUENCE_NUMBER = 1u << 1;
    constexpr std::uint32_t VERSION_EDIT_HAS_CURRENT_WAL_ID = 1u << 2;

    std::uint32_t payload_flags(const VersionEdit::Payload& payload)
    {
        std::uint32_t flags = 0;

        if (payload.next_table_id.has_value()) {
            flags |= VERSION_EDIT_HAS_NEXT_TABLE_ID;
        }

        if (payload.next_sequence_number.has_value()) {
            flags |= VERSION_EDIT_HAS_NEXT_SEQUENCE_NUMBER;
        }

        if (payload.current_wal_id.has_value()) {
            flags |= VERSION_EDIT_HAS_CURRENT_WAL_ID;
        }

        return flags;
    }

    Result<std::uint32_t> checked_u32_count(std::uint64_t value, const char* name)
    {
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            return Result<std::uint32_t>::fail(
                Status{ StatusCode::AllocationTooLarge, std::format("{} {} exceeds uint32_t", name, value) }
            );
        }

        return Result<std::uint32_t>::ok(static_cast<std::uint32_t>(value));
    }

    bool can_ignore_trailing_manifest_error(StatusCode code)
    {
        return code == StatusCode::UnexpectedEOF ||
            code == StatusCode::ChecksumMismatch ||
            code == StatusCode::InvalidPayloadSize ||
            code == StatusCode::Corruption;
    }

} // namespace

void DeletedTable::calculate_crc(std::uint32_t& crc_buffer, bool init) const
{
    if (init) {
        crc_buffer = ::crc32(0, Z_NULL, 0);
    }

    crc32_add_pod<std::uint32_t>(crc_buffer, level);
    crc32_add_pod<std::uint64_t>(crc_buffer, table_id);
}

Result<DeletedTable> DeletedTable::load(ReadableFile& file, std::uint64_t& offset)
{
    DeletedTable table{};

    Status result = kvdb::blockio::read_u32_t_le(file, table.level, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) {
        return Result<DeletedTable>::fail(std::move(result));
    }

    result = kvdb::blockio::read_u64_t_le(file, table.table_id, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) {
        return Result<DeletedTable>::fail(std::move(result));
    }

    return Result<DeletedTable>::ok(std::move(table));
}

Status DeletedTable::write(WritableFile& file, std::uint64_t& offset) const
{
    Result<std::uint64_t> current_position = file.current_position();
    if (!current_position.is_ok()) {
        return std::move(current_position.status);
    }

    if (current_position.value != offset) {
        return Status{
            StatusCode::InvalidOffset,
            std::format(
                "current writable file offset {} and tracked offset {} differ",
                current_position.value,
                offset
            )
        };
    }

    Status result = kvdb::blockio::write_u32_t_le(file, level, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) {
        return result;
    }

    result = kvdb::blockio::write_u64_t_le(file, table_id, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) {
        return result;
    }

    return Status::ok();
}

Status VersionEdit::Header::write(WritableFile& file, std::uint64_t& offset) const
{
    Result<std::uint64_t> current_position = file.current_position();
    if (!current_position.is_ok()) {
        return std::move(current_position.status);
    }

    if (current_position.value != offset) {
        return Status{
            StatusCode::InvalidOffset,
            std::format(
                "current writable file offset {} and tracked offset {} differ",
                current_position.value,
                offset
            )
        };
    }

    Status result = kvdb::blockio::write_u32_t_le(file, crc32, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) {
        return result;
    }

    result = kvdb::blockio::write_u32_t_le(file, payload_size, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) {
        return result;
    }

    return Status::ok();
}

Result<VersionEdit::Header> VersionEdit::Header::load(ReadableFile& file, std::uint64_t& offset)
{
    Header result{};

    Status read_result = kvdb::blockio::read_u32_t_le(file, result.crc32, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) {
        return Result<Header>::fail(std::move(read_result));
    }

    read_result = kvdb::blockio::read_u32_t_le(file, result.payload_size, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) {
        return Result<Header>::fail(std::move(read_result));
    }

    if (result.payload_size == 0) {
        return Result<Header>::fail(
            Status{ StatusCode::InvalidPayloadSize, "version edit payload size is zero" }
        );
    }

    return Result<Header>::ok(std::move(result));
}

std::uint32_t VersionEdit::Payload::disk_size() const
{
    std::uint64_t size = sizeof(std::uint32_t); // flags

    if (next_table_id.has_value()) {
        size += sizeof(std::uint64_t);
    }

    if (next_sequence_number.has_value()) {
        size += sizeof(std::uint64_t);
    }

    if (current_wal_id.has_value()) {
        size += sizeof(std::uint64_t);
    }

    size += sizeof(std::uint32_t); // deleted table count
    size += sizeof(std::uint32_t); // new table count
    size += static_cast<std::uint64_t>(deleted_tables.size()) * DeletedTable::disk_size();

    for (const auto& table : new_tables) {
        size += table.disk_size();
    }

    return static_cast<std::uint32_t>(size);
}

void VersionEdit::Payload::compute_crc32(std::uint32_t& crc_buffer) const
{
    crc_buffer = ::crc32(0, Z_NULL, 0);

    const std::uint32_t flags = payload_flags(*this);
    const std::uint32_t deleted_count = static_cast<std::uint32_t>(deleted_tables.size());
    const std::uint32_t new_count = static_cast<std::uint32_t>(new_tables.size());

    crc32_add_pod<std::uint32_t>(crc_buffer, flags);

    if (next_table_id.has_value()) {
        crc32_add_pod<std::uint64_t>(crc_buffer, *next_table_id);
    }

    if (next_sequence_number.has_value()) {
        crc32_add_pod<std::uint64_t>(crc_buffer, *next_sequence_number);
    }

    if (current_wal_id.has_value()) {
        crc32_add_pod<std::uint64_t>(crc_buffer, *current_wal_id);
    }

    crc32_add_pod<std::uint32_t>(crc_buffer, deleted_count);
    crc32_add_pod<std::uint32_t>(crc_buffer, new_count);

    for (const auto& table : deleted_tables) {
        table.calculate_crc(crc_buffer);
    }

    for (const auto& table : new_tables) {
        table.calculate_crc(crc_buffer);
    }
}

Status VersionEdit::Payload::write(WritableFile& file, std::uint64_t& offset) const
{
    auto deleted_count_result = checked_u32_count(deleted_tables.size(), "deleted table count");
    if (!deleted_count_result.is_ok()) {
        return std::move(deleted_count_result.status);
    }

    auto new_count_result = checked_u32_count(new_tables.size(), "new table count");
    if (!new_count_result.is_ok()) {
        return std::move(new_count_result.status);
    }

    const std::uint32_t flags = payload_flags(*this);
    const std::uint32_t deleted_count = deleted_count_result.value;
    const std::uint32_t new_count = new_count_result.value;

    Status result = kvdb::blockio::write_u32_t_le(file, flags, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) {
        return result;
    }

    if (next_table_id.has_value()) {
        result = kvdb::blockio::write_u64_t_le(file, *next_table_id, offset, MANIFEST_BLOCK_SIZE);
        if (!result.is_ok()) return result;
    }

    if (next_sequence_number.has_value()) {
        result = kvdb::blockio::write_u64_t_le(file, *next_sequence_number, offset, MANIFEST_BLOCK_SIZE);
        if (!result.is_ok()) return result;
    }

    if (current_wal_id.has_value()) {
        result = kvdb::blockio::write_u64_t_le(file, *current_wal_id, offset, MANIFEST_BLOCK_SIZE);
        if (!result.is_ok()) return result;
    }

    result = kvdb::blockio::write_u32_t_le(file, deleted_count, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) return result;

    result = kvdb::blockio::write_u32_t_le(file, new_count, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) return result;

    for (const auto& table : deleted_tables) {
        result = table.write(file, offset);
        if (!result.is_ok()) return result;
    }

    for (const auto& table : new_tables) {
        result = table.write(file, offset);
        if (!result.is_ok()) return result;
    }

    return Status::ok();
}

Result<VersionEdit::Payload> VersionEdit::Payload::load(
    ReadableFile& file,
    std::uint64_t& offset,
    std::uint32_t payload_size,
    Arena& arena
)
{
    const std::uint64_t payload_begin = offset;
    const std::uint64_t payload_end = payload_begin + payload_size;

    Payload result{};
    Status read_result;

    std::uint32_t flags = 0;
    read_result = kvdb::blockio::read_u32_t_le(file, flags, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return Result<Payload>::fail(std::move(read_result));

    const std::uint32_t known_flags =
        VERSION_EDIT_HAS_NEXT_TABLE_ID |
        VERSION_EDIT_HAS_NEXT_SEQUENCE_NUMBER |
        VERSION_EDIT_HAS_CURRENT_WAL_ID;

    if ((flags & ~known_flags) != 0) {
        return Result<Payload>::fail(
            Status{ StatusCode::InvalidPayloadSize, std::format("unknown version edit flags: 0x{:08x}", flags) }
        );
    }

    if ((flags & VERSION_EDIT_HAS_NEXT_TABLE_ID) != 0) {
        std::uint64_t value = 0;
        read_result = kvdb::blockio::read_u64_t_le(file, value, offset, MANIFEST_BLOCK_SIZE);
        if (!read_result.is_ok()) return Result<Payload>::fail(std::move(read_result));
        result.next_table_id = value;
    }

    if ((flags & VERSION_EDIT_HAS_NEXT_SEQUENCE_NUMBER) != 0) {
        std::uint64_t value = 0;
        read_result = kvdb::blockio::read_u64_t_le(file, value, offset, MANIFEST_BLOCK_SIZE);
        if (!read_result.is_ok()) return Result<Payload>::fail(std::move(read_result));
        result.next_sequence_number = value;
    }

    if ((flags & VERSION_EDIT_HAS_CURRENT_WAL_ID) != 0) {
        std::uint64_t value = 0;
        read_result = kvdb::blockio::read_u64_t_le(file, value, offset, MANIFEST_BLOCK_SIZE);
        if (!read_result.is_ok()) return Result<Payload>::fail(std::move(read_result));
        result.current_wal_id = value;
    }

    std::uint32_t deleted_count = 0;
    std::uint32_t new_count = 0;

    read_result = kvdb::blockio::read_u32_t_le(file, deleted_count, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return Result<Payload>::fail(std::move(read_result));

    read_result = kvdb::blockio::read_u32_t_le(file, new_count, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return Result<Payload>::fail(std::move(read_result));

    result.deleted_tables.reserve(deleted_count);
    result.new_tables.reserve(new_count);

    for (std::uint32_t i = 0; i < deleted_count; ++i) {
        Result<DeletedTable> deleted = DeletedTable::load(file, offset);
        if (!deleted.is_ok()) {
            return Result<Payload>::fail(std::move(deleted.status));
        }
        result.deleted_tables.push_back(std::move(deleted.value));
    }

    for (std::uint32_t i = 0; i < new_count; ++i) {
        Result<TableMeta> table = TableMeta::load(file, offset, arena);
        if (!table.is_ok()) {
            return Result<Payload>::fail(std::move(table.status));
        }
        result.new_tables.push_back(std::move(table.value));
    }

    if (offset != payload_end) {
        return Result<Payload>::fail(
            Status{
                StatusCode::InvalidPayloadSize,
                std::format("version edit payload ended at {}, expected {}", offset, payload_end)
            }
        );
    }

    return Result<Payload>::ok(std::move(result));
}

Status VersionEdit::write(WritableFile& file, std::uint64_t& offset) const
{
    VersionEdit copy = *this;
    copy.header.payload_size = copy.payload.disk_size();
    copy.payload.compute_crc32(copy.header.crc32);

    Status result = copy.header.write(file, offset);
    if (!result.is_ok()) {
        return result;
    }

    result = copy.payload.write(file, offset);
    if (!result.is_ok()) {
        return result;
    }

    return Status::ok();
}

Result<VersionEdit> VersionEdit::load(ReadableFile& file, std::uint64_t& offset, Arena& arena)
{
    const std::uint64_t edit_begin = offset;

    Result<Header> header = Header::load(file, offset);
    if (!header.is_ok()) {
        return Result<VersionEdit>::fail(std::move(header.status));
    }

    Result<Payload> payload = Payload::load(file, offset, header.value.payload_size, arena);
    if (!payload.is_ok()) {
        return Result<VersionEdit>::fail(std::move(payload.status));
    }

    std::uint32_t actual_crc = 0;
    payload.value.compute_crc32(actual_crc);

    if (actual_crc != header.value.crc32) {
        return Result<VersionEdit>::fail(
            Status{
                StatusCode::ChecksumMismatch,
                std::format(
                    "manifest version edit crc mismatch at offset {}: expected=0x{:08x} actual=0x{:08x}",
                    edit_begin,
                    header.value.crc32,
                    actual_crc
                )
            }
        );
    }

    VersionEdit edit{};
    edit.header = std::move(header.value);
    edit.payload = std::move(payload.value);

    return Result<VersionEdit>::ok(std::move(edit));
}

Status Manifest::Header::compute_crc32()
{
    crc32 = ::crc32(0, Z_NULL, 0);

    crc32_add_pod<std::uint32_t>(crc32, magic);
    crc32_add_pod<std::uint32_t>(crc32, version);
    crc32_add_pod<std::uint32_t>(crc32, header_size);
    crc32_add_pod<std::uint32_t>(crc32, flags);
    crc32_add_pod<std::uint32_t>(crc32, reserved);

    return Status::ok();
}

Status Manifest::Header::write(WritableFile& file, std::uint64_t& offset) const
{
    Result<std::uint64_t> current_position = file.current_position();
    if (!current_position.is_ok()) {
        return std::move(current_position.status);
    }

    if (current_position.value != offset) {
        return Status{
            StatusCode::InvalidOffset,
            std::format(
                "current writable file offset {} and tracked offset {} differ",
                current_position.value,
                offset
            )
        };
    }

    Status result = kvdb::blockio::write_u32_t_le(file, magic, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) return result;

    result = kvdb::blockio::write_u32_t_le(file, version, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) return result;

    result = kvdb::blockio::write_u32_t_le(file, header_size, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) return result;

    result = kvdb::blockio::write_u32_t_le(file, flags, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) return result;

    result = kvdb::blockio::write_u32_t_le(file, reserved, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) return result;

    result = kvdb::blockio::write_u32_t_le(file, crc32, offset, MANIFEST_BLOCK_SIZE);
    if (!result.is_ok()) return result;

    return Status::ok();
}

Result<Manifest::Header> Manifest::Header::load(ReadableFile& file, std::uint64_t& offset)
{
    Header result{};
    Status read_result;

    read_result = kvdb::blockio::read_u32_t_le(file, result.magic, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return Result<Header>::fail(std::move(read_result));

    if (result.magic != MANIFEST_MAGIC) {
        return Result<Header>::fail(
            Status{
                StatusCode::BadMagic,
                std::format("invalid manifest magic: expected=0x{:08x} actual=0x{:08x}", MANIFEST_MAGIC, result.magic)
            }
        );
    }

    read_result = kvdb::blockio::read_u32_t_le(file, result.version, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return Result<Header>::fail(std::move(read_result));

    if (result.version != MANIFEST_VERSION) {
        return Result<Header>::fail(
            Status{
                StatusCode::UnsupportedVersion,
                std::format("unsupported manifest version {}; supported version is {}", result.version, MANIFEST_VERSION)
            }
        );
    }

    read_result = kvdb::blockio::read_u32_t_le(file, result.header_size, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return Result<Header>::fail(std::move(read_result));

    if (result.header_size != Header::disk_size()) {
        return Result<Header>::fail(
            Status{
                StatusCode::InvalidHeader,
                std::format("manifest header size {}; expected {}", result.header_size, Header::disk_size())
            }
        );
    }

    read_result = kvdb::blockio::read_u32_t_le(file, result.flags, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return Result<Header>::fail(std::move(read_result));

    read_result = kvdb::blockio::read_u32_t_le(file, result.reserved, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return Result<Header>::fail(std::move(read_result));

    read_result = kvdb::blockio::read_u32_t_le(file, result.crc32, offset, MANIFEST_BLOCK_SIZE);
    if (!read_result.is_ok()) return Result<Header>::fail(std::move(read_result));

    Header expected = result;
    expected.compute_crc32();

    if (expected.crc32 != result.crc32) {
        return Result<Header>::fail(
            Status{
                StatusCode::ChecksumMismatch,
                std::format(
                    "manifest header crc mismatch: expected=0x{:08x} actual=0x{:08x}",
                    result.crc32,
                    expected.crc32
                )
            }
        );
    }

    return Result<Header>::ok(std::move(result));
}

Manifest::Manifest(std::filesystem::path path)
    : path_(std::move(path))
{
}

Result<Manifest> Manifest::load(const std::filesystem::path& path, Arena& arena)
{
    Result<std::unique_ptr<ReadableFile>> readable_result = open_readable_file(path);
    if (!readable_result.is_ok()) {
        return Result<Manifest>::fail(std::move(readable_result.status));
    }

    std::unique_ptr<ReadableFile> readable = std::move(readable_result.value);

    std::uint64_t file_size = 0;
    Status size_status = readable->get_file_size(file_size);
    if (!size_status.is_ok()) {
        return Result<Manifest>::fail(std::move(size_status));
    }

    std::uint64_t offset = 0;

    Result<Header> header = Header::load(*readable, offset);
    if (!header.is_ok()) {
        return Result<Manifest>::fail(std::move(header.status));
    }

    Manifest manifest(path);
    manifest.header_ = std::move(header.value);
    manifest.readable_ = std::move(readable);
    manifest.append_offset_ = offset;

    while (offset < file_size) {
        const std::uint64_t edit_begin = offset;
        Result<VersionEdit> edit = VersionEdit::load(*manifest.readable_, offset, arena);

        if (!edit.is_ok()) {
            if (can_ignore_trailing_manifest_error(edit.status.code)) {
                manifest.append_offset_ = edit_begin;
                break;
            }

            return Result<Manifest>::fail(std::move(edit.status));
        }

        Status apply_status = manifest.apply(edit.value, arena);
        if (!apply_status.is_ok()) {
            return Result<Manifest>::fail(std::move(apply_status));
        }

        manifest.append_offset_ = offset;
    }

    Status invariant_status = manifest.check_invariants();
    if (!invariant_status.is_ok()) {
        return Result<Manifest>::fail(std::move(invariant_status));
    }

    return Result<Manifest>::ok(std::move(manifest));
}

Status Manifest::open_or_create()
{
    if (path_.empty()) {
        return Status{ StatusCode::InvalidArgument, "manifest path is empty" };
    }

    if (std::filesystem::exists(path_) && std::filesystem::file_size(path_) != 0) {
        return Status{
            StatusCode::AlreadyExists,
            "manifest already exists; use Manifest::load(path, arena) for recovery, then reopen with an append-capable WritableFile"
        };
    }

    Result<std::unique_ptr<WritableFile>> writable_result = open_writable_file(path_);
    if (!writable_result.is_ok()) {
        return std::move(writable_result.status);
    }

    writable_ = std::move(writable_result.value);
    append_offset_ = 0;

    header_ = Header{};
    Status crc_status = header_.compute_crc32();
    if (!crc_status.is_ok()) {
        return crc_status;
    }

    Status write_status = header_.write(*writable_, append_offset_);
    if (!write_status.is_ok()) {
        return write_status;
    }

    return sync();
}

Status Manifest::append(const VersionEdit& edit)
{
    if (!writable_) {
        return Status{ StatusCode::FailedPrecondition, "manifest is not open for writing" };
    }

    Result<std::uint64_t> current_position = writable_->current_position();
    if (!current_position.is_ok()) {
        return std::move(current_position.status);
    }

    if (current_position.value != append_offset_) {
        return Status{
            StatusCode::InvalidOffset,
            std::format(
                "manifest append offset {} differs from writable current position {}",
                append_offset_,
                current_position.value
            )
        };
    }

    return edit.write(*writable_, append_offset_);
}

Status Manifest::apply(const VersionEdit& edit, Arena& arena)
{
    (void)arena;

    std::uint64_t max_table_id_seen = 0;

    for (const auto& deleted : edit.payload.deleted_tables) {
        Status remove_status = level_manager_.remove_table(deleted.table_id, deleted.level);
        if (!remove_status.is_ok()) {
            return remove_status;
        }

        max_table_id_seen = std::max(max_table_id_seen, deleted.table_id);
    }

    for (const auto& table : edit.payload.new_tables) {
        TableMeta copy = table;
        Status add_status = level_manager_.add_table(std::move(copy));
        if (!add_status.is_ok()) {
            return add_status;
        }

        max_table_id_seen = std::max(max_table_id_seen, table.table_id);
    }

    if (edit.payload.next_table_id.has_value()) {
        next_table_id_ = *edit.payload.next_table_id;
    }
    else if (max_table_id_seen >= next_table_id_) {
        next_table_id_ = max_table_id_seen + 1;
    }

    if (edit.payload.next_sequence_number.has_value()) {
        next_sequence_number_ = *edit.payload.next_sequence_number;
    }

    if (edit.payload.current_wal_id.has_value()) {
        current_wal_id_ = *edit.payload.current_wal_id;
    }

    return check_invariants();
}

Status Manifest::commit(const VersionEdit& edit, Arena& arena)
{
    Status append_status = append(edit);
    if (!append_status.is_ok()) {
        return append_status;
    }

    Status sync_status = sync();
    if (!sync_status.is_ok()) {
        return sync_status;
    }

    return apply(edit, arena);
}

Status Manifest::sync()
{
    if (!writable_) {
        return Status{ StatusCode::FailedPrecondition, "manifest is not open for writing" };
    }

    return writable_->sync();
}

std::uint64_t Manifest::allocate_table_id()
{
    return next_table_id_++;
}

std::uint64_t Manifest::current_wal_id() const
{
    return current_wal_id_;
}

std::uint64_t Manifest::last_sequence_number() const
{
    if (next_sequence_number_ == 0) {
        return 0;
    }

    return next_sequence_number_ - 1;
}

std::uint64_t Manifest::next_table_id() const
{
    return next_table_id_;
}

const LevelManager& Manifest::level_manager() const
{
    return level_manager_;
}

LevelManager& Manifest::mutable_level_manager()
{
    return level_manager_;
}

Status Manifest::check_invariants() const
{
    if (next_table_id_ == 0) {
        return Status{ StatusCode::InvariantViolation, "next_table_id must never be zero" };
    }

    if (current_wal_id_ == 0) {
        return Status{ StatusCode::InvariantViolation, "current_wal_id must never be zero" };
    }

    for (std::uint32_t level = 0; level < level_manager_.level_count(); ++level) {
        const std::vector<TableMeta>* tables = level_manager_.get_lx_tables(level);
        if (tables == nullptr) {
            return Status{ StatusCode::InvariantViolation, std::format("level {} disappeared", level) };
        }

        for (const auto& table : *tables) {
            if (table.level != level) {
                return Status{
                    StatusCode::InvariantViolation,
                    std::format("table {} stored in level {}, but meta says level {}", table.table_id, level, table.level)
                };
            }

            if (table.table_id >= next_table_id_) {
                return Status{
                    StatusCode::InvariantViolation,
                    std::format("table id {} is not below next_table_id {}", table.table_id, next_table_id_)
                };
            }

            if (table.largest_key < table.smallest_key) {
                return Status{
                    StatusCode::InvariantViolation,
                    std::format("table {} has invalid key range", table.table_id)
                };
            }
        }
    }

    return Status::ok();
}
