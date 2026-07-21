#include "sstable_entities/file_header_section.h"

#include <format>
#include <utility>

using namespace SSTableEntities;

namespace
{
    Status validate_header_fields(const FileHeaderSection& header)
    {
        if (header.magic != FILE_HEADER_MAGIC) {
            return Status{
                StatusCode::BadMagic,
                std::format(
                    "invalid SSTable header magic: expected=0x{:08x}, actual=0x{:08x}",
                    FILE_HEADER_MAGIC,
                    header.magic
                )
            };
        }

        // Version 0 has never been a valid SSTable version. Versions newer than
        // this reader understands must also be rejected.
        if (header.version == 0 || header.version > LATEST_SSTABLE_VERSION) {
            return Status{
                StatusCode::UnsupportedVersion,
                std::format(
                    "unsupported SSTable version: file_version={}, latest_supported_version={}",
                    header.version,
                    LATEST_SSTABLE_VERSION
                )
            };
        }

        if (header.block_size != BLOCK_SIZE) {
            return Status{
                StatusCode::UnsupportedBlockSize,
                std::format(
                    "unsupported SSTable block size: file_block_size={}, expected_block_size={}",
                    header.block_size,
                    BLOCK_SIZE
                )
            };
        }

        return Status::ok();
    }
}

Status FileHeaderSection::write(WritableFile& file, std::uint64_t& offset)
{
    // Reject an invalid in-memory header before adding padding or writing bytes.
    Status status = validate_header_fields(*this);
    if (!status.is_ok()) {
        return status;
    }

    status = calculate_crc32(crc32);
    if (!status.is_ok()) {
        return status;
    }

    status = align_to_block_boundary(file, offset, BLOCK_SIZE);
    if (!status.is_ok()) {
        return status;
    }

    const std::uint64_t header_offset = offset;

    const auto write_u32 = [&](std::uint32_t value) -> Status {
        return kvdb::blockio::write_u32_t_le(file, value, offset, BLOCK_SIZE);
        };

    for (const std::uint32_t value : {
        magic,
            version,
            flags,
            block_size,
            table_id,
            crc32
    }) {
        status = write_u32(value);
        if (!status.is_ok()) {
            // Do not reset offset here. A failed append may already have written
            // bytes, so offset must continue to describe the physical cursor.
            return status;
        }
    }

    Result<std::uint64_t> position = file.current_position();
    if (!position.is_ok()) {
        return std::move(position.status);
    }

    if (position.value != offset || offset - header_offset != disk_size()) {
        return Status{
            StatusCode::InvariantViolation,
            "SSTable header write ended at an unexpected file position"
        };
    }

    return Status::ok();
}

Result<FileHeaderSection> FileHeaderSection::load(
    ReadableFile& file,
    std::uint64_t& offset
)
{
    const std::uint64_t initial_offset = offset;


    const auto fail = [&](Status status) -> Result<FileHeaderSection> {
        offset = initial_offset;
        return Result<FileHeaderSection>::fail(std::move(status));
        };

    Status status = align_to_block_boundary(file, offset, BLOCK_SIZE);
    if (!status.is_ok()) {
        return fail(std::move(status));
    }

    const std::uint64_t header_offset = offset;
    FileHeaderSection result{};

    const auto read_u32 = [&](std::uint32_t& value) -> Status {
        return kvdb::blockio::read_u32_t_le(file, value, offset, BLOCK_SIZE);
        };

    for (std::uint32_t* field : {
        &result.magic,
        &result.version,
        &result.flags,
        &result.block_size,
        &result.table_id,
        &result.crc32
        }) {
        status = read_u32(*field);
        if (!status.is_ok()) {
            return fail(std::move(status));
        }
    }

    if (offset - header_offset != disk_size()) {
        return fail(Status{
            StatusCode::InvariantViolation,
            "SSTable header load consumed an unexpected number of bytes"
            });
    }

    if (result.magic != FILE_HEADER_MAGIC) {
        return fail(Status{
            StatusCode::BadMagic,
            std::format(
                "invalid SSTable header magic: expected=0x{:08x}, actual=0x{:08x}",
                FILE_HEADER_MAGIC,
                result.magic
            )
            });
    }

    std::uint32_t expected_crc32 = 0;
    status = result.calculate_crc32(expected_crc32);
    if (!status.is_ok()) {
        return fail(std::move(status));
    }

    // Verify integrity before interpreting mutable fields such as version and
    // block size. A flipped version bit is corruption, not a genuine new format.
    if (expected_crc32 != result.crc32) {
        return fail(Status{
            StatusCode::ChecksumMismatch,
            std::format(
                "SSTable header CRC mismatch: expected={}, actual={}",
                expected_crc32,
                result.crc32
            )
            });
    }

    status = validate_header_fields(result);
    if (!status.is_ok()) {
        return fail(std::move(status));
    }

    return Result<FileHeaderSection>::ok(std::move(result));
}

Status FileHeaderSection::calculate_crc32(
    std::uint32_t& crc_buffer
) const noexcept
{
    crc_buffer = ::crc32(0L, Z_NULL, 0);
    crc32_add_pod(crc_buffer, magic);
    crc32_add_pod(crc_buffer, version);
    crc32_add_pod(crc_buffer, flags);
    crc32_add_pod(crc_buffer, block_size);
    crc32_add_pod(crc_buffer, table_id);
    return Status::ok();
}

FileHeaderSection::FileHeaderSection() noexcept
    : FileHeaderSection(0)
{
}

FileHeaderSection::FileHeaderSection(std::uint32_t table_id_value) noexcept
    : table_id(table_id_value)
{
    const Status status = calculate_crc32(crc32);
    (void)status;
}