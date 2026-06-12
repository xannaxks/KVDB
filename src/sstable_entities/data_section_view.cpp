#include "sstable_entities/data_section_view.h"

using namespace SSTableEntities;

Result<DataSectionView> DataSectionView::load(
    ReadableFile& file,
    std::uint64_t& offset,
    const std::uint64_t& first_data_block_offset,
    std::uint32_t data_block_count
)
{
    if (first_data_block_offset == 0 && data_block_count > 0)
        return Result<DataSectionView>::fail(
            Status{
                StatusCode::InvalidAlignment,
                "First data block offset invalid"
            }
        );

    if (first_data_block_offset != 0)
    {
        if (first_data_block_offset % BLOCK_SIZE != 0)
            return Result<DataSectionView>::fail(
                Status{
                    StatusCode::InvalidAlignment,
                    "First data block offset not aligned to block size"
                }
            );

        offset = first_data_block_offset;
    }

    DataSectionView result{};

    result.data_blocks.reserve(data_block_count);

    while (data_block_count--)
    {
        auto data_block = DataSectionView::DataBlock::load(file, offset);
        if (!data_block.is_ok())
            return Result<DataSectionView>::fail(std::move(data_block.status));

        result.data_blocks.emplace_back(std::move(data_block.value));
    }

    return Result<DataSectionView>::ok(std::move(result));
}
Result<DataSectionView::Header> DataSectionView::Header::load(ReadableFile& file, std::uint64_t& offset)
{
    Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);
    if (!align_to_block_result.is_ok())
        return Result<Header>::fail(std::move(align_to_block_result));

    Header result{};
    result.header_offset = offset;

    uint8_t tmp_type;

    Status read_endian_result;

    read_endian_result = std::move(kvdb::blockio::read_u8_t(file, tmp_type, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<Header>::fail(std::move(read_endian_result));

    result.header.type = static_cast<BlockType>(tmp_type);
    if (result.header.type != BlockType::Data)
        return Result<Header>::fail(
            Status{
                StatusCode::InvalidBlockType,
                "Expected Data block type in DataSection"
            }
        );

    read_endian_result = std::move(kvdb::blockio::read_u32_t_le(file, result.header.payload_disk_size, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<Header>::fail(std::move(read_endian_result));

    read_endian_result = std::move(kvdb::blockio::read_u32_t_le(file, result.header.crc32, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<Header>::fail(std::move(read_endian_result));

    if (result.header.payload_disk_size > BLOCK_SIZE - DataSection::Header::disk_size())
        return Result<Header>::fail(
            Status{
                StatusCode::InvalidPayloadSize,
                std::format(
                    "data block payload exceeds block capacity during data section view header reading: payload_size={}, capacity={}",
                    result.header.payload_disk_size,
                    BLOCK_SIZE - DataSection::Header::disk_size()
                )
            }
        );

    result.payload_offset = offset;
    result.next_block_offset = result.header_offset + BLOCK_SIZE;

    if (result.payload_offset + result.header.payload_disk_size > result.next_block_offset)
        return Result<Header>::fail(
            Status{
                StatusCode::OffsetOverlap,
                "Current payload overlaps with next block"
            }
        );

    return Result<Header>::ok(std::move(result));
}

Result<DataSectionView::DataBlock>
DataSectionView::DataBlock::load(ReadableFile& file, std::uint64_t& offset)
{
    //assert(static_cast<std::uint64_t>(file.tellg()) == offset);

    Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);
    if (!align_to_block_result.is_ok())
        return Result<DataBlock>::fail(std::move(align_to_block_result));

    DataSectionView::DataBlock result{};

    auto header_res = DataSectionView::Header::load(file, offset);
    if (!header_res.is_ok())
        return Result<DataBlock>::fail(std::move(header_res.status));

    result.header_view = std::move(header_res.value);

    // Lazy loading: do not read payload.
    // Just jump to the next physical data block.
    offset = result.header_view.next_block_offset;

    //file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    //if (!file)
    //    return std::nullopt;

    return Result<DataBlock>::ok(std::move(result));
}