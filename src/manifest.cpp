#include "manifest.h"

#include "crc32_helpers.h"
#include "endian_io.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <limits>
#include <new>
#include <utility>

namespace {

    constexpr std::uint32_t VERSION_EDIT_HAS_NEXT_TABLE_ID = 1u << 0;
    constexpr std::uint32_t VERSION_EDIT_HAS_NEXT_SEQUENCE_NUMBER = 1u << 1;
    constexpr std::uint32_t VERSION_EDIT_HAS_CURRENT_WAL_ID = 1u << 2;
    constexpr std::uint32_t VERSION_EDIT_KNOWN_FLAGS =
        VERSION_EDIT_HAS_NEXT_TABLE_ID |
        VERSION_EDIT_HAS_NEXT_SEQUENCE_NUMBER |
        VERSION_EDIT_HAS_CURRENT_WAL_ID;

    constexpr std::uint32_t MIN_PAYLOAD_SIZE =
        4u + // flags
        4u + // deleted count
        4u;  // new count

    [[nodiscard]] std::uint32_t payload_flags(
        const VersionEdit::Payload& payload
    ) noexcept
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

    [[nodiscard]] Status checked_add_u64(
        std::uint64_t& destination,
        std::uint64_t value,
        const char* what
    )
    {
        if (value > std::numeric_limits<std::uint64_t>::max() - destination) {
            return Status{
                StatusCode::AllocationTooLarge,
                std::format("{} size overflow", what)
            };
        }

        destination += value;
        return Status::ok();
    }

    [[nodiscard]] Result<std::uint32_t> checked_u32_count(
        std::size_t value,
        const char* name
    )
    {
        if (value > MANIFEST_MAX_TABLES_PER_EDIT) {
            return Result<std::uint32_t>::fail(
                Status{
                    StatusCode::AllocationTooLarge,
                    std::format(
                        "{} {} exceeds manifest limit {}",
                        name,
                        value,
                        MANIFEST_MAX_TABLES_PER_EDIT
                    )
                }
            );
        }

        if (value > std::numeric_limits<std::uint32_t>::max()) {
            return Result<std::uint32_t>::fail(
                Status{
                    StatusCode::AllocationTooLarge,
                    std::format("{} {} exceeds uint32_t", name, value)
                }
            );
        }

        return Result<std::uint32_t>::ok(
            static_cast<std::uint32_t>(value)
        );
    }

    [[nodiscard]] Status require_remaining(
        std::uint64_t offset,
        std::uint64_t end,
        std::uint64_t bytes,
        const char* field
    )
    {
        if (offset > end || bytes > end - offset) {
            return Status{
                StatusCode::InvalidPayloadSize,
                std::format(
                    "{} does not fit in declared version-edit payload",
                    field
                )
            };
        }

        return Status::ok();
    }

} // namespace

void DeletedTable::calculate_crc(
    std::uint32_t& crc_buffer,
    bool init
) const
{
    if (init) {
        ::init_crc_buff(crc_buffer);
    }

    crc32_add_pod<std::uint32_t>(crc_buffer, level);
    crc32_add_pod<std::uint64_t>(crc_buffer, table_id);
}

Result<DeletedTable> DeletedTable::load(
    ReadableFile& file,
    std::uint64_t& offset
)
{
    DeletedTable table{};

    Status status = kvdb::blockio::read_u32_t_le(
        file,
        table.level,
        offset,
        MANIFEST_BLOCK_SIZE
    );
    if (!status.is_ok()) {
        return Result<DeletedTable>::fail(std::move(status));
    }

    status = kvdb::blockio::read_u64_t_le(
        file,
        table.table_id,
        offset,
        MANIFEST_BLOCK_SIZE
    );
    if (!status.is_ok()) {
        return Result<DeletedTable>::fail(std::move(status));
    }

    return Result<DeletedTable>::ok(std::move(table));
}

Status DeletedTable::write(
    WritableFile& file,
    std::uint64_t& offset
) const
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

    Status status = kvdb::blockio::write_u32_t_le(
        file,
        level,
        offset,
        MANIFEST_BLOCK_SIZE
    );
    if (!status.is_ok()) {
        return status;
    }

    return kvdb::blockio::write_u64_t_le(
        file,
        table_id,
        offset,
        MANIFEST_BLOCK_SIZE
    );
}

Status VersionEdit::Header::write(
    WritableFile& file,
    std::uint64_t& offset
) const
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

    Status status = kvdb::blockio::write_u32_t_le(
        file,
        crc32,
        offset,
        MANIFEST_BLOCK_SIZE
    );
    if (!status.is_ok()) {
        return status;
    }

    return kvdb::blockio::write_u32_t_le(
        file,
        payload_size,
        offset,
        MANIFEST_BLOCK_SIZE
    );
}

Result<VersionEdit::Header> VersionEdit::Header::load(
    ReadableFile& file,
    std::uint64_t& offset
)
{
    Header result{};

    Status status = kvdb::blockio::read_u32_t_le(
        file,
        result.crc32,
        offset,
        MANIFEST_BLOCK_SIZE
    );
    if (!status.is_ok()) {
        return Result<Header>::fail(std::move(status));
    }

    status = kvdb::blockio::read_u32_t_le(
        file,
        result.payload_size,
        offset,
        MANIFEST_BLOCK_SIZE
    );
    if (!status.is_ok()) {
        return Result<Header>::fail(std::move(status));
    }

    if (result.payload_size < MIN_PAYLOAD_SIZE) {
        return Result<Header>::fail(
            Status{
                StatusCode::InvalidPayloadSize,
                std::format(
                    "version edit payload size {} is below minimum {}",
                    result.payload_size,
                    MIN_PAYLOAD_SIZE
                )
            }
        );
    }

    if (result.payload_size > MANIFEST_MAX_EDIT_SIZE) {
        return Result<Header>::fail(
            Status{
                StatusCode::InvalidPayloadSize,
                std::format(
                    "version edit payload size {} exceeds limit {}",
                    result.payload_size,
                    MANIFEST_MAX_EDIT_SIZE
                )
            }
        );
    }

    return Result<Header>::ok(std::move(result));
}

Status VersionEdit::Payload::validate() const
{
    auto deleted_count = checked_u32_count(
        deleted_tables.size(),
        "deleted table count"
    );
    if (!deleted_count.is_ok()) {
        return std::move(deleted_count.status);
    }

    auto new_count = checked_u32_count(
        new_tables.size(),
        "new table count"
    );
    if (!new_count.is_ok()) {
        return std::move(new_count.status);
    }

    if (next_table_id.has_value() && *next_table_id == 0) {
        return Status{
            StatusCode::InvalidArgument,
            "next_table_id cannot be zero"
        };
    }

    if (next_sequence_number.has_value() &&
        *next_sequence_number == 0) {
        return Status{
            StatusCode::InvalidArgument,
            "next_sequence_number cannot be zero"
        };
    }

    if (current_wal_id.has_value() && *current_wal_id == 0) {
        return Status{
            StatusCode::InvalidArgument,
            "current_wal_id cannot be zero"
        };
    }

    for (const auto& deleted : deleted_tables) {
        if (deleted.table_id == 0) {
            return Status{
                StatusCode::InvalidArgument,
                "deleted table id cannot be zero"
            };
        }
    }

    for (const auto& table : new_tables) {
        if (table.table_id == 0) {
            return Status{
                StatusCode::InvalidArgument,
                "new table id cannot be zero"
            };
        }
    }

    return Status::ok();
}

Result<std::uint32_t> VersionEdit::Payload::encoded_size() const
{
    Status validation = validate();
    if (!validation.is_ok()) {
        return Result<std::uint32_t>::fail(std::move(validation));
    }

    std::uint64_t size = MIN_PAYLOAD_SIZE;

    if (next_table_id.has_value()) {
        size += 8u;
    }
    if (next_sequence_number.has_value()) {
        size += 8u;
    }
    if (current_wal_id.has_value()) {
        size += 8u;
    }

    const std::uint64_t deleted_bytes =
        static_cast<std::uint64_t>(deleted_tables.size()) *
        DeletedTable::disk_size();

    Status status = checked_add_u64(
        size,
        deleted_bytes,
        "version edit payload"
    );
    if (!status.is_ok()) {
        return Result<std::uint32_t>::fail(std::move(status));
    }

    for (const auto& table : new_tables) {
        status = checked_add_u64(
            size,
            static_cast<std::uint64_t>(table.disk_size()),
            "version edit payload"
        );
        if (!status.is_ok()) {
            return Result<std::uint32_t>::fail(std::move(status));
        }
    }

    if (size > MANIFEST_MAX_EDIT_SIZE ||
        size > std::numeric_limits<std::uint32_t>::max()) {
        return Result<std::uint32_t>::fail(
            Status{
                StatusCode::AllocationTooLarge,
                std::format(
                    "version edit payload size {} exceeds supported maximum",
                    size
                )
            }
        );
    }

    return Result<std::uint32_t>::ok(
        static_cast<std::uint32_t>(size)
    );
}

Status VersionEdit::Payload::compute_crc32(
    std::uint32_t& crc_buffer
) const
{
    Status validation = validate();
    if (!validation.is_ok()) {
        return validation;
    }

    const auto deleted_count =
        static_cast<std::uint32_t>(deleted_tables.size());
    const auto new_count =
        static_cast<std::uint32_t>(new_tables.size());
    const std::uint32_t flags = payload_flags(*this);

    ::init_crc_buff(crc_buffer);
    crc32_add_pod<std::uint32_t>(crc_buffer, flags);

    if (next_table_id.has_value()) {
        crc32_add_pod<std::uint64_t>(crc_buffer, *next_table_id);
    }
    if (next_sequence_number.has_value()) {
        crc32_add_pod<std::uint64_t>(
            crc_buffer,
            *next_sequence_number
        );
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

    return Status::ok();
}

Status VersionEdit::Payload::write(
    WritableFile& file,
    std::uint64_t& offset
) const
{
    auto size_result = encoded_size();
    if (!size_result.is_ok()) {
        return std::move(size_result.status);
    }

    const std::uint64_t payload_begin = offset;
    const std::uint32_t flags = payload_flags(*this);
    const auto deleted_count =
        static_cast<std::uint32_t>(deleted_tables.size());
    const auto new_count =
        static_cast<std::uint32_t>(new_tables.size());

    Status status = kvdb::blockio::write_u32_t_le(
        file,
        flags,
        offset,
        MANIFEST_BLOCK_SIZE
    );
    if (!status.is_ok()) {
        return status;
    }

    if (next_table_id.has_value()) {
        status = kvdb::blockio::write_u64_t_le(
            file,
            *next_table_id,
            offset,
            MANIFEST_BLOCK_SIZE
        );
        if (!status.is_ok()) {
            return status;
        }
    }

    if (next_sequence_number.has_value()) {
        status = kvdb::blockio::write_u64_t_le(
            file,
            *next_sequence_number,
            offset,
            MANIFEST_BLOCK_SIZE
        );
        if (!status.is_ok()) {
            return status;
        }
    }

    if (current_wal_id.has_value()) {
        status = kvdb::blockio::write_u64_t_le(
            file,
            *current_wal_id,
            offset,
            MANIFEST_BLOCK_SIZE
        );
        if (!status.is_ok()) {
            return status;
        }
    }

    status = kvdb::blockio::write_u32_t_le(
        file,
        deleted_count,
        offset,
        MANIFEST_BLOCK_SIZE
    );
    if (!status.is_ok()) {
        return status;
    }

    status = kvdb::blockio::write_u32_t_le(
        file,
        new_count,
        offset,
        MANIFEST_BLOCK_SIZE
    );
    if (!status.is_ok()) {
        return status;
    }

    for (const auto& table : deleted_tables) {
        status = table.write(file, offset);
        if (!status.is_ok()) {
            return status;
        }
    }

    for (const auto& table : new_tables) {
        status = table.write(file, offset);
        if (!status.is_ok()) {
            return status;
        }
    }

    if (offset - payload_begin != size_result.value) {
        return Status{
            StatusCode::InvariantViolation,
            std::format(
                "payload writer produced {} bytes; encoded size is {}",
                offset - payload_begin,
                size_result.value
            )
        };
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
    if (payload_size < MIN_PAYLOAD_SIZE ||
        payload_size > MANIFEST_MAX_EDIT_SIZE) {
        return Result<Payload>::fail(
            Status{
                StatusCode::InvalidPayloadSize,
                std::format(
                    "invalid version edit payload size {}",
                    payload_size
                )
            }
        );
    }

    if (payload_size >
        std::numeric_limits<std::uint64_t>::max() - offset) {
        return Result<Payload>::fail(
            Status{
                StatusCode::InvalidPayloadSize,
                "version edit payload end overflows uint64_t"
            }
        );
    }

    const std::uint64_t payload_end = offset + payload_size;
    Payload result{};

    auto read_u32 = [&](std::uint32_t& value, const char* field) -> Status {
        Status bound = require_remaining(offset, payload_end, 4u, field);
        if (!bound.is_ok()) {
            return bound;
        }
        return kvdb::blockio::read_u32_t_le(
            file,
            value,
            offset,
            MANIFEST_BLOCK_SIZE
        );
        };

    auto read_u64 = [&](std::uint64_t& value, const char* field) -> Status {
        Status bound = require_remaining(offset, payload_end, 8u, field);
        if (!bound.is_ok()) {
            return bound;
        }
        return kvdb::blockio::read_u64_t_le(
            file,
            value,
            offset,
            MANIFEST_BLOCK_SIZE
        );
        };

    std::uint32_t flags = 0;
    Status status = read_u32(flags, "flags");
    if (!status.is_ok()) {
        return Result<Payload>::fail(std::move(status));
    }

    if ((flags & ~VERSION_EDIT_KNOWN_FLAGS) != 0) {
        return Result<Payload>::fail(
            Status{
                StatusCode::Corruption,
                std::format(
                    "unknown version edit flags: 0x{:08x}",
                    flags
                )
            }
        );
    }

    if ((flags & VERSION_EDIT_HAS_NEXT_TABLE_ID) != 0) {
        std::uint64_t value = 0;
        status = read_u64(value, "next_table_id");
        if (!status.is_ok()) {
            return Result<Payload>::fail(std::move(status));
        }
        result.next_table_id = value;
    }

    if ((flags & VERSION_EDIT_HAS_NEXT_SEQUENCE_NUMBER) != 0) {
        std::uint64_t value = 0;
        status = read_u64(value, "next_sequence_number");
        if (!status.is_ok()) {
            return Result<Payload>::fail(std::move(status));
        }
        result.next_sequence_number = value;
    }

    if ((flags & VERSION_EDIT_HAS_CURRENT_WAL_ID) != 0) {
        std::uint64_t value = 0;
        status = read_u64(value, "current_wal_id");
        if (!status.is_ok()) {
            return Result<Payload>::fail(std::move(status));
        }
        result.current_wal_id = value;
    }

    std::uint32_t deleted_count = 0;
    std::uint32_t new_count = 0;

    status = read_u32(deleted_count, "deleted table count");
    if (!status.is_ok()) {
        return Result<Payload>::fail(std::move(status));
    }

    status = read_u32(new_count, "new table count");
    if (!status.is_ok()) {
        return Result<Payload>::fail(std::move(status));
    }

    if (deleted_count > MANIFEST_MAX_TABLES_PER_EDIT ||
        new_count > MANIFEST_MAX_TABLES_PER_EDIT) {
        return Result<Payload>::fail(
            Status{
                StatusCode::AllocationTooLarge,
                "version edit table count exceeds configured safety limit"
            }
        );
    }

    const std::uint64_t remaining = payload_end - offset;
    const std::uint64_t deleted_bytes =
        static_cast<std::uint64_t>(deleted_count) *
        DeletedTable::disk_size();

    if (deleted_bytes > remaining) {
        return Result<Payload>::fail(
            Status{
                StatusCode::InvalidPayloadSize,
                "deleted table array does not fit in payload"
            }
        );
    }

    // At least one encoded byte per new table is necessary. The exact table
    // boundary must additionally be enforced by TableMeta::load itself.
    if (static_cast<std::uint64_t>(new_count) >
        remaining - deleted_bytes) {
        return Result<Payload>::fail(
            Status{
                StatusCode::InvalidPayloadSize,
                "new table count cannot fit in payload"
            }
        );
    }

    try {
        result.deleted_tables.reserve(deleted_count);
        result.new_tables.reserve(new_count);
    }
    catch (const std::bad_alloc&) {
        return Result<Payload>::fail(
            Status{
                StatusCode::AllocationTooLarge,
                "could not allocate version edit table arrays"
            }
        );
    }

    for (std::uint32_t i = 0; i < deleted_count; ++i) {
        status = require_remaining(
            offset,
            payload_end,
            DeletedTable::disk_size(),
            "deleted table"
        );
        if (!status.is_ok()) {
            return Result<Payload>::fail(std::move(status));
        }

        Result<DeletedTable> deleted = DeletedTable::load(file, offset);
        if (!deleted.is_ok()) {
            return Result<Payload>::fail(std::move(deleted.status));
        }
        result.deleted_tables.push_back(std::move(deleted.value));
    }

    for (std::uint32_t i = 0; i < new_count; ++i) {
        if (offset >= payload_end) {
            return Result<Payload>::fail(
                Status{
                    StatusCode::InvalidPayloadSize,
                    "new table starts outside payload"
                }
            );
        }

        Result<TableMeta> table = TableMeta::load(file, offset, arena);
        if (!table.is_ok()) {
            return Result<Payload>::fail(std::move(table.status));
        }

        if (offset > payload_end) {
            return Result<Payload>::fail(
                Status{
                    StatusCode::InvalidPayloadSize,
                    "TableMeta crossed the version-edit payload boundary"
                }
            );
        }

        result.new_tables.push_back(std::move(table.value));
    }

    if (offset != payload_end) {
        return Result<Payload>::fail(
            Status{
                StatusCode::InvalidPayloadSize,
                std::format(
                    "version edit payload ended at {}, expected {}",
                    offset,
                    payload_end
                )
            }
        );
    }

    status = result.validate();
    if (!status.is_ok()) {
        return Result<Payload>::fail(std::move(status));
    }

    return Result<Payload>::ok(std::move(result));
}

Result<std::uint32_t> VersionEdit::encoded_size() const
{
    auto payload_size = payload.encoded_size();
    if (!payload_size.is_ok()) {
        return Result<std::uint32_t>::fail(
            std::move(payload_size.status)
        );
    }

    if (payload_size.value >
        std::numeric_limits<std::uint32_t>::max() - Header::disk_size()) {
        return Result<std::uint32_t>::fail(
            Status{
                StatusCode::AllocationTooLarge,
                "version edit encoded size exceeds uint32_t"
            }
        );
    }

    return Result<std::uint32_t>::ok(
        Header::disk_size() + payload_size.value
    );
}

Status VersionEdit::add_table(const TableMeta& meta)
{
    try {
        payload.new_tables.push_back(meta);
    }
    catch (const std::bad_alloc&) {
        return Status{
            StatusCode::AllocationTooLarge,
            "could not append table metadata to version edit"
        };
    }

    return Status::ok();
}

Status VersionEdit::prepare()
{
    auto payload_size = payload.encoded_size();
    if (!payload_size.is_ok()) {
        return std::move(payload_size.status);
    }

    std::uint32_t crc = 0;
    Status crc_status = payload.compute_crc32(crc);
    if (!crc_status.is_ok()) {
        return crc_status;
    }

    header.payload_size = payload_size.value;
    header.crc32 = crc;
    return Status::ok();
}

Status VersionEdit::write(
    WritableFile& file,
    std::uint64_t& offset
)
{
    Status prepare_status = prepare();
    if (!prepare_status.is_ok()) {
        return prepare_status;
    }

    const std::uint64_t edit_begin = offset;

    Status status = header.write(file, offset);
    if (!status.is_ok()) {
        return status;
    }

    status = payload.write(file, offset);
    if (!status.is_ok()) {
        return status;
    }

    const std::uint64_t expected_size =
        Header::disk_size() +
        static_cast<std::uint64_t>(header.payload_size);

    if (offset - edit_begin != expected_size) {
        return Status{
            StatusCode::InvariantViolation,
            std::format(
                "version edit writer produced {} bytes; expected {}",
                offset - edit_begin,
                expected_size
            )
        };
    }

    return Status::ok();
}

Result<VersionEdit> VersionEdit::load(
    ReadableFile& file,
    std::uint64_t& offset,
    std::uint64_t file_size,
    Arena& arena
)
{
    const std::uint64_t edit_begin = offset;

    if (offset > file_size ||
        VersionEdit::Header::disk_size() > file_size - offset) {
        return Result<VersionEdit>::fail(
            Status{
                StatusCode::UnexpectedEOF,
                "truncated version edit header"
            }
        );
    }

    Result<Header> header = Header::load(file, offset);
    if (!header.is_ok()) {
        return Result<VersionEdit>::fail(std::move(header.status));
    }

    if (header.value.payload_size > file_size - offset) {
        return Result<VersionEdit>::fail(
            Status{
                StatusCode::UnexpectedEOF,
                std::format(
                    "truncated version edit at offset {}: payload needs {} bytes, only {} remain",
                    edit_begin,
                    header.value.payload_size,
                    file_size - offset
                )
            }
        );
    }

    Result<Payload> payload = Payload::load(
        file,
        offset,
        header.value.payload_size,
        arena
    );
    if (!payload.is_ok()) {
        // The complete declared byte range exists. EOF from a nested parser is
        // therefore structural corruption, not a recoverable torn tail.
        if (payload.status.code == StatusCode::UnexpectedEOF) {
            return Result<VersionEdit>::fail(
                Status{
                    StatusCode::Corruption,
                    std::format(
                        "version edit at offset {} is structurally truncated inside a complete declared record",
                        edit_begin
                    )
                }
            );
        }
        return Result<VersionEdit>::fail(std::move(payload.status));
    }

    std::uint32_t actual_crc = 0;
    Status crc_status = payload.value.compute_crc32(actual_crc);
    if (!crc_status.is_ok()) {
        return Result<VersionEdit>::fail(std::move(crc_status));
    }

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
    ::init_crc_buff(crc32);
    crc32_add_pod<std::uint32_t>(crc32, magic);
    crc32_add_pod<std::uint32_t>(crc32, version);
    crc32_add_pod<std::uint32_t>(crc32, header_size);
    crc32_add_pod<std::uint32_t>(crc32, flags);
    crc32_add_pod<std::uint32_t>(crc32, reserved);
    return Status::ok();
}

Status Manifest::Header::write(
    WritableFile& file,
    std::uint64_t& offset
) const
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

    Status status = kvdb::blockio::write_u32_t_le(
        file, magic, offset, MANIFEST_BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u32_t_le(
        file, version, offset, MANIFEST_BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u32_t_le(
        file, header_size, offset, MANIFEST_BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u32_t_le(
        file, flags, offset, MANIFEST_BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u32_t_le(
        file, reserved, offset, MANIFEST_BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    return kvdb::blockio::write_u32_t_le(
        file, crc32, offset, MANIFEST_BLOCK_SIZE
    );
}

Result<Manifest::Header> Manifest::Header::load(
    ReadableFile& file,
    std::uint64_t& offset
)
{
    Header result{};
    Status status;

    status = kvdb::blockio::read_u32_t_le(
        file, result.magic, offset, MANIFEST_BLOCK_SIZE
    );
    if (!status.is_ok()) return Result<Header>::fail(std::move(status));

    if (result.magic != MANIFEST_MAGIC) {
        return Result<Header>::fail(
            Status{
                StatusCode::BadMagic,
                std::format(
                    "invalid manifest magic: expected=0x{:08x} actual=0x{:08x}",
                    MANIFEST_MAGIC,
                    result.magic
                )
            }
        );
    }

    status = kvdb::blockio::read_u32_t_le(
        file, result.version, offset, MANIFEST_BLOCK_SIZE
    );
    if (!status.is_ok()) return Result<Header>::fail(std::move(status));

    if (result.version != MANIFEST_VERSION) {
        return Result<Header>::fail(
            Status{
                StatusCode::UnsupportedVersion,
                std::format(
                    "unsupported manifest version {}; supported version is {}",
                    result.version,
                    MANIFEST_VERSION
                )
            }
        );
    }

    status = kvdb::blockio::read_u32_t_le(
        file, result.header_size, offset, MANIFEST_BLOCK_SIZE
    );
    if (!status.is_ok()) return Result<Header>::fail(std::move(status));

    if (result.header_size != Header::disk_size()) {
        return Result<Header>::fail(
            Status{
                StatusCode::InvalidHeader,
                std::format(
                    "manifest header size {}; expected {}",
                    result.header_size,
                    Header::disk_size()
                )
            }
        );
    }

    status = kvdb::blockio::read_u32_t_le(
        file, result.flags, offset, MANIFEST_BLOCK_SIZE
    );
    if (!status.is_ok()) return Result<Header>::fail(std::move(status));

    status = kvdb::blockio::read_u32_t_le(
        file, result.reserved, offset, MANIFEST_BLOCK_SIZE
    );
    if (!status.is_ok()) return Result<Header>::fail(std::move(status));

    if (result.flags != 0 || result.reserved != 0) {
        return Result<Header>::fail(
            Status{
                StatusCode::InvalidHeader,
                std::format(
                    "manifest v1 requires flags=0 and reserved=0; got flags={} reserved={}",
                    result.flags,
                    result.reserved
                )
            }
        );
    }

    status = kvdb::blockio::read_u32_t_le(
        file, result.crc32, offset, MANIFEST_BLOCK_SIZE
    );
    if (!status.is_ok()) return Result<Header>::fail(std::move(status));

    Header expected = result;
    status = expected.compute_crc32();
    if (!status.is_ok()) {
        return Result<Header>::fail(std::move(status));
    }

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

Result<Manifest> Manifest::load(
    LevelManager& level_manager,
    const std::filesystem::path& path,
    Arena& arena
)
{
    Result<std::unique_ptr<ReadableFile>> readable_result =
        open_readable_file(path);
    if (!readable_result.is_ok()) {
        return Result<Manifest>::fail(
            std::move(readable_result.status)
        );
    }

    std::unique_ptr<ReadableFile> readable =
        std::move(readable_result.value);

    std::uint64_t file_size = 0;
    Status size_status = readable->get_file_size(file_size);
    if (!size_status.is_ok()) {
        return Result<Manifest>::fail(std::move(size_status));
    }

    if (file_size < Header::disk_size()) {
        return Result<Manifest>::fail(
            Status{
                StatusCode::UnexpectedEOF,
                std::format(
                    "manifest is {} bytes; header requires {}",
                    file_size,
                    Header::disk_size()
                )
            }
        );
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

    LevelManager recovered(level_manager.level_count());
    StagedCounters recovered_counters{};

    while (offset < file_size) {
        const std::uint64_t edit_begin = offset;

        Result<VersionEdit> edit = VersionEdit::load(
            *manifest.readable_,
            offset,
            file_size,
            arena
        );

        if (!edit.is_ok()) {
            if (edit.status.code == StatusCode::UnexpectedEOF) {
                // Only an incomplete final record is recoverable. A complete
                // record with bad CRC or bad structure is corruption.
                manifest.recoverable_tail_ = true;
                manifest.append_offset_ = edit_begin;
                offset = edit_begin;
                break;
            }

            return Result<Manifest>::fail(std::move(edit.status));
        }

        Status apply_status = stage_apply(
            recovered,
            recovered_counters,
            edit.value
        );
        if (!apply_status.is_ok()) {
            return Result<Manifest>::fail(std::move(apply_status));
        }

        manifest.append_offset_ = offset;
    }

    Status invariant_status = check_invariants(
        recovered,
        recovered_counters
    );
    if (!invariant_status.is_ok()) {
        return Result<Manifest>::fail(std::move(invariant_status));
    }

    level_manager.swap(recovered);
    manifest.publish_counters(recovered_counters);
    return Result<Manifest>::ok(std::move(manifest));
}

Status Manifest::open_or_create()
{
    if (path_.empty()) {
        return Status{
            StatusCode::InvalidArgument,
            "manifest path is empty"
        };
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(path_, error);
    if (error) {
        return Status{
            StatusCode::OpenFailed,
            std::format(
                "could not inspect manifest path {}: {}",
                path_.string(),
                error.message()
            )
        };
    }

    if (exists) {
        const std::uintmax_t size =
            std::filesystem::file_size(path_, error);
        if (error) {
            return Status{
                StatusCode::OpenFailed,
                std::format(
                    "could not read manifest size {}: {}",
                    path_.string(),
                    error.message()
                )
            };
        }

        if (size != 0) {
            return Status{
                StatusCode::AlreadyExists,
                "manifest already exists; recover it with Manifest::load"
            };
        }
    }

    Result<std::unique_ptr<WritableFile>> writable_result =
        open_writable_file(path_);
    if (!writable_result.is_ok()) {
        return std::move(writable_result.status);
    }

    writable_ = std::move(writable_result.value);
    readable_.reset();
    append_offset_ = 0;
    recoverable_tail_ = false;
    write_poisoned_ = false;

    header_ = Header{};
    Status status = header_.compute_crc32();
    if (!status.is_ok()) {
        return status;
    }

    status = header_.write(*writable_, append_offset_);
    if (!status.is_ok()) {
        write_poisoned_ = true;
        return status;
    }

    status = writable_->sync();
    if (!status.is_ok()) {
        write_poisoned_ = true;
        return status;
    }

    return Status::ok();
}

Status Manifest::prepare_for_append()
{
    if (write_poisoned_) {
        return Status{
            StatusCode::FailedPrecondition,
            "manifest has an uncertain partial write; reload before appending"
        };
    }

    writable_.reset();
    readable_.reset();

    if (!recoverable_tail_) {
        return Status::ok();
    }

    std::error_code error;
    std::filesystem::resize_file(path_, append_offset_, error);
    if (error) {
        return Status{
            StatusCode::OpenFailed,
            std::format(
                "could not truncate manifest {} to {}: {}",
                path_.string(),
                append_offset_,
                error.message()
            )
        };
    }

    recoverable_tail_ = false;
    return Status::ok();
}

Status Manifest::attach_writer(
    std::unique_ptr<WritableFile> writable
)
{
    if (write_poisoned_) {
        return Status{
            StatusCode::FailedPrecondition,
            "manifest has an uncertain partial write; reload before attaching a writer"
        };
    }

    if (recoverable_tail_) {
        return Status{
            StatusCode::FailedPrecondition,
            "truncate the recoverable manifest tail before attaching a writer"
        };
    }

    if (!writable) {
        return Status{
            StatusCode::InvalidArgument,
            "cannot attach a null manifest writer"
        };
    }

    Result<std::uint64_t> position = writable->current_position();
    if (!position.is_ok()) {
        return std::move(position.status);
    }

    if (position.value != append_offset_) {
        return Status{
            StatusCode::InvalidOffset,
            std::format(
                "attached writer is at {}, manifest append offset is {}",
                position.value,
                append_offset_
            )
        };
    }

    readable_.reset();
    writable_ = std::move(writable);
    return Status::ok();
}

Manifest::StagedCounters Manifest::counters() const noexcept
{
    return StagedCounters{
        next_table_id_,
        current_wal_id_,
        next_sequence_number_
    };
}

void Manifest::publish_counters(
    const StagedCounters& counters
) noexcept
{
    next_table_id_ = counters.next_table_id;
    current_wal_id_ = counters.current_wal_id;
    next_sequence_number_ = counters.next_sequence_number;
}

Status Manifest::stage_apply(
    LevelManager& level_manager,
    StagedCounters& counters,
    const VersionEdit& edit
)
{
    Status payload_status = edit.payload.validate();
    if (!payload_status.is_ok()) {
        return payload_status;
    }

    std::uint64_t max_table_id_seen = 0;

    for (const auto& deleted : edit.payload.deleted_tables) {
        Status status = level_manager.remove_table(
            deleted.table_id,
            deleted.level
        );
        if (!status.is_ok()) {
            return status;
        }
        max_table_id_seen = std::max(
            max_table_id_seen,
            deleted.table_id
        );
    }

    for (const auto& table : edit.payload.new_tables) {
        TableMeta copy = table;
        Status status = level_manager.add_table(std::move(copy));
        if (!status.is_ok()) {
            return status;
        }
        max_table_id_seen = std::max(
            max_table_id_seen,
            table.table_id
        );
    }

    if (edit.payload.next_table_id.has_value()) {
        const std::uint64_t value = *edit.payload.next_table_id;
        if (value < counters.next_table_id) {
            return Status{
                StatusCode::InvariantViolation,
                std::format(
                    "next_table_id regressed from {} to {}",
                    counters.next_table_id,
                    value
                )
            };
        }
        counters.next_table_id = value;
    }
    else if (max_table_id_seen >= counters.next_table_id) {
        if (max_table_id_seen ==
            std::numeric_limits<std::uint64_t>::max()) {
            return Status{
                StatusCode::InvariantViolation,
                "table id space is exhausted"
            };
        }
        counters.next_table_id = max_table_id_seen + 1;
    }

    if (edit.payload.next_sequence_number.has_value()) {
        const std::uint64_t value =
            *edit.payload.next_sequence_number;
        if (value < counters.next_sequence_number) {
            return Status{
                StatusCode::InvariantViolation,
                std::format(
                    "next_sequence_number regressed from {} to {}",
                    counters.next_sequence_number,
                    value
                )
            };
        }
        counters.next_sequence_number = value;
    }

    if (edit.payload.current_wal_id.has_value()) {
        const std::uint64_t value = *edit.payload.current_wal_id;
        if (value < counters.current_wal_id) {
            return Status{
                StatusCode::InvariantViolation,
                std::format(
                    "current_wal_id regressed from {} to {}",
                    counters.current_wal_id,
                    value
                )
            };
        }
        counters.current_wal_id = value;
    }

    return check_invariants(level_manager, counters);
}

Status Manifest::check_invariants(
    const LevelManager& level_manager,
    const StagedCounters& counters
)
{
    if (counters.next_table_id == 0) {
        return Status{
            StatusCode::InvariantViolation,
            "next_table_id must never be zero"
        };
    }

    if (counters.current_wal_id == 0) {
        return Status{
            StatusCode::InvariantViolation,
            "current_wal_id must never be zero"
        };
    }

    if (counters.next_sequence_number == 0) {
        return Status{
            StatusCode::InvariantViolation,
            "next_sequence_number must never be zero"
        };
    }

    for (std::uint32_t level = 0;
        level < level_manager.level_count();
        ++level) {
        const std::vector<TableMeta>* tables =
            level_manager.get_lx_tables(level);
        if (tables == nullptr) {
            return Status{
                StatusCode::InvariantViolation,
                std::format("level {} disappeared", level)
            };
        }

        for (std::size_t i = 0; i < tables->size(); ++i) {
            const auto& table = (*tables)[i];

            if (table.table_id == 0) {
                return Status{
                    StatusCode::InvariantViolation,
                    "installed table id cannot be zero"
                };
            }

            if (table.level != level) {
                return Status{
                    StatusCode::InvariantViolation,
                    std::format(
                        "table {} stored in level {}, but meta says level {}",
                        table.table_id,
                        level,
                        table.level
                    )
                };
            }

            if (table.table_id >= counters.next_table_id) {
                return Status{
                    StatusCode::InvariantViolation,
                    std::format(
                        "table id {} is not below next_table_id {}",
                        table.table_id,
                        counters.next_table_id
                    )
                };
            }

            if (table.largest_key < table.smallest_key) {
                return Status{
                    StatusCode::InvariantViolation,
                    std::format(
                        "table {} has invalid key range",
                        table.table_id
                    )
                };
            }

            if (level == 0 && i != 0 &&
                (*tables)[i - 1].table_id <= table.table_id) {
                return Status{
                    StatusCode::InvariantViolation,
                    "L0 tables are not ordered newest first"
                };
            }

            if (level > 0 && i != 0) {
                const auto& previous = (*tables)[i - 1];
                if (!(previous.largest_key < table.smallest_key)) {
                    return Status{
                        StatusCode::InvariantViolation,
                        std::format(
                            "tables {} and {} overlap or are out of order on level {}",
                            previous.table_id,
                            table.table_id,
                            level
                        )
                    };
                }
            }
        }
    }

    return Status::ok();
}

Status Manifest::apply(
    LevelManager& level_manager,
    const VersionEdit& edit
)
{
    try {
        LevelManager staged = level_manager;
        StagedCounters staged_counters = counters();

        Status status = stage_apply(
            staged,
            staged_counters,
            edit
        );
        if (!status.is_ok()) {
            return status;
        }

        level_manager.swap(staged);
        publish_counters(staged_counters);
        return Status::ok();
    }
    catch (const std::bad_alloc&) {
        return Status{
            StatusCode::AllocationTooLarge,
            "could not stage manifest edit"
        };
    }
}

Status Manifest::append_prepared(VersionEdit& edit)
{
    if (write_poisoned_) {
        return Status{
            StatusCode::FailedPrecondition,
            "manifest writer is poisoned; reload before writing"
        };
    }

    if (!writable_) {
        return Status{
            StatusCode::FailedPrecondition,
            "manifest is not open for writing"
        };
    }

    Result<std::uint64_t> current_position =
        writable_->current_position();
    if (!current_position.is_ok()) {
        write_poisoned_ = true;
        return std::move(current_position.status);
    }

    if (current_position.value != append_offset_) {
        write_poisoned_ = true;
        return Status{
            StatusCode::InvalidOffset,
            std::format(
                "manifest append offset {} differs from writable current position {}",
                append_offset_,
                current_position.value
            )
        };
    }

    Status status = edit.write(*writable_, append_offset_);
    if (!status.is_ok()) {
        // offset/file position may now point into a partially written record.
        write_poisoned_ = true;
        return status;
    }

    return Status::ok();
}

Status Manifest::commit(
    LevelManager& level_manager,
    VersionEdit& edit
)
{
    if (write_poisoned_) {
        return Status{
            StatusCode::FailedPrecondition,
            "manifest writer is poisoned; reload before committing"
        };
    }

    if (!writable_) {
        return Status{
            StatusCode::FailedPrecondition,
            "manifest is not open for writing"
        };
    }

    try {
        LevelManager staged = level_manager;
        StagedCounters staged_counters = counters();

        Status status = stage_apply(
            staged,
            staged_counters,
            edit
        );
        if (!status.is_ok()) {
            return status;
        }

        status = edit.prepare();
        if (!status.is_ok()) {
            return status;
        }

        status = append_prepared(edit);
        if (!status.is_ok()) {
            return status;
        }

        status = writable_->sync();
        if (!status.is_ok()) {
            // Durability is uncertain. Never retry on this object because that
            // could append the same logical edit twice.
            write_poisoned_ = true;
            return status;
        }

        level_manager.swap(staged);
        publish_counters(staged_counters);
        return Status::ok();
    }
    catch (const std::bad_alloc&) {
        return Status{
            StatusCode::AllocationTooLarge,
            "could not stage manifest commit"
        };
    }
}

Status Manifest::sync()
{
    if (write_poisoned_) {
        return Status{
            StatusCode::FailedPrecondition,
            "manifest writer is poisoned; reload before syncing"
        };
    }

    if (!writable_) {
        return Status{
            StatusCode::FailedPrecondition,
            "manifest is not open for writing"
        };
    }

    Status status = writable_->sync();
    if (!status.is_ok()) {
        write_poisoned_ = true;
    }
    return status;
}

Result<std::uint64_t> Manifest::allocate_table_id()
{
    if (next_table_id_ ==
        std::numeric_limits<std::uint64_t>::max()) {
        return Result<std::uint64_t>::fail(
            Status{
                StatusCode::InvariantViolation,
                "table id space is exhausted"
            }
        );
    }

    const std::uint64_t allocated = next_table_id_;
    ++next_table_id_;
    return Result<std::uint64_t>::ok(allocated);
}

std::uint64_t Manifest::current_wal_id() const noexcept
{
    return current_wal_id_;
}

std::uint64_t Manifest::next_sequence_number() const noexcept
{
    return next_sequence_number_;
}

std::uint64_t Manifest::next_table_id() const noexcept
{
    return next_table_id_;
}

std::uint64_t Manifest::append_offset() const noexcept
{
    return append_offset_;
}

bool Manifest::has_recoverable_tail() const noexcept
{
    return recoverable_tail_;
}

bool Manifest::write_poisoned() const noexcept
{
    return write_poisoned_;
}