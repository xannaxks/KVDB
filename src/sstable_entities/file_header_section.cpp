#include "sstable_entities/file_header_section.h"
#include <cassert>
#include <format>

using namespace SSTableEntities;

Status FileHeaderSection::write(WritableFile& file, std::uint64_t& offset)
{
    Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);
    if (!align_to_block_result.is_ok())
        return std::move(align_to_block_result);

    Status write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->magic, offset, BLOCK_SIZE);;
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->version, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->flags, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->block_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->table_id, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->crc32, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    Result<std::uint64_t> get_position_result = file.current_position();
    if (!get_position_result.is_ok())
        return std::move(get_position_result.status);

    assert(get_position_result.value < BLOCK_SIZE);

    return Status::ok();
}

Result<FileHeaderSection> FileHeaderSection::load(ReadableFile& file, std::uint64_t& offset)
{
    // assert(file.current_ == offset);

    Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);
    if (!align_to_block_result.is_ok())
        return Result<FileHeaderSection>::fail(std::move(align_to_block_result));

    FileHeaderSection result{};
    std::uint64_t file_header_offset = offset;

    Status magic_status = kvdb::blockio::read_u32_t_le(file, result.magic, offset, BLOCK_SIZE);
    if (!magic_status.is_ok())
        return Result<FileHeaderSection>::fail(std::move(magic_status));
    if (result.magic != FILE_HEADER_MAGIC)
        return Result<FileHeaderSection>::fail(
            Status{
                StatusCode::BadMagic,
                std::format(
                    "Invalid SSTable file header section magic: expected=0x{:08x} actual=0x{:08x}",
                    FILE_HEADER_MAGIC, result.magic
                )
            }
        );

    Status read_endian_result;

    read_endian_result = std::move(kvdb::blockio::read_u32_t_le(file, result.version, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<FileHeaderSection>::fail(std::move(read_endian_result));

    if (result.version != SSTABLE_VERSION)
        return Result<FileHeaderSection>::fail(
            Status{
                StatusCode::UnsupportedVersion,
                std::format("unsupported SSTable version: file_version={}, supported_version={}",
                   result.version, SSTABLE_VERSION)
            }
        );

    read_endian_result = std::move(kvdb::blockio::read_u32_t_le(file, result.flags, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<FileHeaderSection>::fail(std::move(read_endian_result));

    read_endian_result = std::move(kvdb::blockio::read_u32_t_le(file, result.block_size, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<FileHeaderSection>::fail(std::move(read_endian_result));

    read_endian_result = std::move(kvdb::blockio::read_u32_t_le(file, result.table_id, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<FileHeaderSection>::fail(std::move(read_endian_result));

    read_endian_result = std::move(kvdb::blockio::read_u32_t_le(file, result.crc32, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<FileHeaderSection>::fail(std::move(read_endian_result));

    std::uint32_t must_be_crc32;
    result.calculate_crc32(must_be_crc32);

    if (must_be_crc32 != result.crc32)
        return Result<FileHeaderSection>::fail(
            Status{
                StatusCode::ChecksumMismatch,
                std::format("SSTable header CRC mismatch: expected={}, actual={}",
                   must_be_crc32, result.crc32)
            }
        );
    if (result.block_size != BLOCK_SIZE)
        return Result<FileHeaderSection>::fail(
            Status{
                StatusCode::UnsupportedBlockSize,
                std::format(
                    "unsupported SSTable block size found in file header section during loading: file_block_size={}, expected_block_size={}",
                    result.block_size,
                    BLOCK_SIZE
                )
            }
        );
    return Result<FileHeaderSection>::ok(std::move(result));
}

Status FileHeaderSection::calculate_crc32(std::uint32_t& crc_buffer)
{
    crc_buffer = ::crc32(0L, Z_NULL, 0);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->magic);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->version);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->flags);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->block_size);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->table_id);

    return Status::ok();
}



std::size_t FileHeaderSection::disk_size()
{
    return (
        sizeof(magic) +
        sizeof(version) +
        sizeof(flags) +
        sizeof(block_size) +
        sizeof(table_id) +
        sizeof(crc32)
        );
}


FileHeaderSection::FileHeaderSection() noexcept
    : FileHeaderSection(0)
{
}

FileHeaderSection::FileHeaderSection(std::uint32_t table_id) noexcept
    : magic(FILE_HEADER_MAGIC),
    version(SSTABLE_VERSION),
    flags(0),
    block_size(BLOCK_SIZE),
    table_id(table_id),
    crc32(0)
{
    crc32 = ::crc32(0L, Z_NULL, 0);
    crc32_add_pod<std::uint32_t>(crc32, this->magic);
    crc32_add_pod<std::uint32_t>(crc32, this->version);
    crc32_add_pod<std::uint32_t>(crc32, this->flags);
    crc32_add_pod<std::uint32_t>(crc32, this->block_size);
    crc32_add_pod<std::uint32_t>(crc32, this->table_id);
}

