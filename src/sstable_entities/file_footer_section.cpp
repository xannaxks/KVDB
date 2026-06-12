#include "sstable_entities/file_footer_section.h"

using namespace SSTableEntities;

Status FileFooterSection::write(WritableFile& file, std::uint64_t& offset)
{
    Result<std::uint64_t> get_position_result = file.current_position();
    if (!get_position_result.is_ok())
        return std::move(get_position_result.status);

    assert(get_position_result.value == offset);
    assert(offset % BLOCK_SIZE == 0);

    Status write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->magic, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->version, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->reserved, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->data_offset, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->data_block_count, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->index_offset, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->index_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->bloom_offset, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->bloom_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->meta_offset, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->meta_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->file_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->footer_crc32, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    return Status::ok();
}

void FileFooterSection::finalize(WritableFile& file, std::uint64_t current_offset)
{
    this->file_size = current_offset + FileFooterSection::disk_size();
    this->calculate_crc32(this->footer_crc32);
}

Result<FileFooterSection> FileFooterSection::load(
    ReadableFile& file,
    std::uint64_t& offset,
    std::uint64_t file_footer_backwards_offset
) {
    if (file_footer_backwards_offset)
    {
        std::uint64_t file_size;

        Status get_file_size_result = file.get_file_size(file_size);

        if (!get_file_size_result.is_ok() or file_size == 0)
            return Result<FileFooterSection>::fail(std::move(get_file_size_result));

        if (file_size < file_footer_backwards_offset)
            return Result<FileFooterSection>::fail(
                Status{
                    StatusCode::InvariantViolation,
                    "File footer backwards offset is greater than file size during file footer loading"
                }
            );

        offset = file_size - file_footer_backwards_offset;
    }

    if (offset % BLOCK_SIZE != 0)
        return Result<FileFooterSection>::fail(
            Status{
                StatusCode::InvariantViolation,
                "File footer offset is not aligned to block size during file footer loading"
            }
        );

    FileFooterSection result{};
    Status read_endian_result;

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.magic, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    if (result.magic != SSTableEntities::FILE_FOOTER_MAGIC)
        return Result<FileFooterSection>::fail(
            Status{
                StatusCode::BadMagic,
                std::format(
                    "File footer magic number is invalid during file footer loading: expected=0x{:08x} actual=0x{:08x}",
                    FILE_FOOTER_MAGIC,
                    result.magic
                )
            }
        );

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.version, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    if (result.version != SSTABLE_VERSION)
        return Result<FileFooterSection>::fail(
            Status{
                StatusCode::UnsupportedVersion,
                std::format("SSTable version is not relevant during sstable file footer section load: file_version={}, supported_version={}",
                    result.version, SSTABLE_VERSION)
            }
        );

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.reserved, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.data_offset, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.data_block_count, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.index_offset, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.index_size, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.bloom_offset, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.bloom_size, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.meta_offset, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.meta_size, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.file_size, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    std::uint64_t actual_file_size;

    Status get_file_size_result = file.get_file_size(actual_file_size);

    if (!get_file_size_result.is_ok() || actual_file_size == 0)
        return Result<FileFooterSection>::fail(std::move(get_file_size_result));

    if (result.file_size != static_cast<std::uint64_t>(actual_file_size))
        return Result<FileFooterSection>::fail(
            Status{
                StatusCode::InvariantViolation,
                "Sizes of file doesn't coresponds, actual one with found in file footer"
            }
        );

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.footer_crc32, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    std::uint32_t must_be_footer_crc32;
    result.calculate_crc32(must_be_footer_crc32);

    if (must_be_footer_crc32 != result.footer_crc32)
        return Result<FileFooterSection>::fail(
            Status{
                StatusCode::ChecksumMismatch,
                std::format("SSTable footer CRC mismatch: expected={}, actual={}",
                   must_be_footer_crc32, result.footer_crc32)
            }
        );

    return Result<FileFooterSection>::ok(std::move(result));
}

void FileFooterSection::calculate_crc32(std::uint32_t& crc_buffer)
{
    crc_buffer = ::crc32(0L, Z_NULL, 0);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->magic);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->version);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->reserved);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->data_offset);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->data_block_count);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->index_offset);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->index_size);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->bloom_offset);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->bloom_size);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->meta_offset);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->meta_size);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->file_size);
}

void FileFooterSection::rebuild(IndexSection& index_block, std::uint64_t index_offset)
{
    this->index_offset = index_offset;
    this->index_size = static_cast<std::uint32_t>(index_block.disk_size());
}
void FileFooterSection::rebuild(BloomSection& bloom_block, std::uint64_t bloom_offset)
{
    this->bloom_offset = bloom_offset;
    this->bloom_size = static_cast<std::uint32_t>(BloomSection::disk_size());
}
void FileFooterSection::rebuild(MetaSection& meta_section, std::uint64_t meta_offset)
{
    this->meta_offset = meta_offset;
    this->meta_size = static_cast<std::uint32_t>(meta_section.disk_size());
}
FileFooterSection::FileFooterSection()
    : magic(FILE_FOOTER_MAGIC),
    version(SSTABLE_VERSION),
    reserved(0),
    data_offset(0),
    data_block_count(0),
    index_offset(0),
    index_size(0),
    bloom_offset(0),
    bloom_size(0),
    meta_offset(0),
    meta_size(0),
    file_size(0),
    footer_crc32(0)
{
    this->calculate_crc32(footer_crc32);
}

std::size_t FileFooterSection::disk_size()
{
    return (
        sizeof(magic) +
        sizeof(version) +
        sizeof(reserved) +
        sizeof(index_offset) +
        sizeof(index_size) +
        sizeof(bloom_offset) +
        sizeof(bloom_size) +
        sizeof(meta_offset) +
        sizeof(meta_size) +
        sizeof(file_size) +
        sizeof(footer_crc32) +
        sizeof(data_offset) +
        sizeof(data_block_count)
        );
}

