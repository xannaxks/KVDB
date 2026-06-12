#include "sstabel_iterator.h"

using namespace SSTableEntities;

SSTableIterator::SSTableIterator(
    const SSTable& sstable,
    std::unique_ptr<ReadableFile>&& file,
    Arena& arena
) :
    sstable(sstable),
    file(std::move(file)),
    arena(arena),
    current_offset(sstable.file_footer_section.data_offset),
    data_block_count(sstable.file_footer_section.data_block_count),
    next_block_index(0),
    record_index(0),
    valid_(false),
    status_(std::move(Status::ok()))
{
}

Status SSTableIterator::next()
{
    if (!valid_)
        return Status::ok();

    ++record_index;

    if (record_index < current_block_records.size())
        return Status::ok();

    status_ = load_next_block();
    return status_;
}

bool SSTableIterator::valid() const
{
    return this->valid_;
}
Status SSTableIterator::status() const
{
    return this->status_;
}

const InternalRecord& SSTableIterator::record() const
{
    assert(valid_);
    assert(record_index < current_block_records.size());

    return current_block_records[record_index];
}

Status SSTableIterator::load_next_block()
{
    this->record_index = 0;
    valid_ = false;
    this->current_block_records.clear();

    if (this->next_block_index >= data_block_count)
        return Status::ok();

    const std::uint64_t block_start = this->current_offset;

    if (block_start % BLOCK_SIZE != 0) {
        return Status{
            StatusCode::InvalidBlockAlignment,
            "Data block offset is not aligned"
        };
    }

    const std::uint64_t block_end = block_start + BLOCK_SIZE;

    DataSection::DataBlock data_block{};
    Status read_result;

    std::uint8_t block_type = 0;

    read_result = kvdb::blockio::read_u8_t(
        *this->file,
        block_type,
        this->current_offset,
        BLOCK_SIZE
    );
    if (!read_result.is_ok())
        return read_result;

    if (static_cast<BlockType>(block_type) != BlockType::Data) {
        return Status{
            StatusCode::InvalidBlockType,
            "Block type mismatch during reading at offset " +
                std::to_string(block_start)
        };
    }

    data_block.header.type = static_cast<BlockType>(block_type);

    read_result = kvdb::blockio::read_u32_t_le(
        *this->file,
        data_block.header.payload_disk_size,
        this->current_offset,
        BLOCK_SIZE
    );
    if (!read_result.is_ok())
        return read_result;

    std::uint32_t expected_crc = 0;

    read_result = kvdb::blockio::read_u32_t_le(
        *this->file,
        expected_crc,
        this->current_offset,
        BLOCK_SIZE
    );
    if (!read_result.is_ok())
        return read_result;

    if (data_block.header.payload_disk_size >
        BLOCK_SIZE - DataSection::Header::disk_size())
    {
        return Status{
            StatusCode::InvalidPayloadSize,
            "Data block payload size exceeds block capacity"
        };
    }

    const std::uint64_t payload_start = this->current_offset;
    const std::uint64_t payload_end =
        payload_start + data_block.header.payload_disk_size;

    if (payload_end > block_end) {
        return Status{
            StatusCode::OffsetOverlap,
            "Data block payload overlaps block boundary"
        };
    }

    while (this->current_offset < payload_end)
    {
        if (payload_end - this->current_offset < DataSection::Payload::fixed_part_disk_size()) {
            return Status{
                StatusCode::Corruption,
                "Not enough bytes for payload fixed part"
            };
        }

        DataSection::Payload payload{};

        const std::uint64_t payload_record_start = this->current_offset;

        read_result = kvdb::blockio::read_u32_t_le(
            *this->file,
            payload.key_size,
            this->current_offset,
            BLOCK_SIZE
        );
        if (!read_result.is_ok())
            return read_result;

        read_result = kvdb::blockio::read_u32_t_le(
            *this->file,
            payload.value_size,
            this->current_offset,
            BLOCK_SIZE
        );
        if (!read_result.is_ok())
            return read_result;

        std::uint8_t type = 0;

        read_result = kvdb::blockio::read_u8_t(
            *this->file,
            type,
            this->current_offset,
            BLOCK_SIZE
        );
        if (!read_result.is_ok())
            return read_result;

        payload.type = static_cast<::Type>(type);

        read_result = kvdb::blockio::read_u32_t_le(
            *this->file,
            payload.flags,
            this->current_offset,
            BLOCK_SIZE
        );
        if (!read_result.is_ok())
            return read_result;

        read_result = kvdb::blockio::read_u32_t_le(
            *this->file,
            payload.reserved,
            this->current_offset,
            BLOCK_SIZE
        );
        if (!read_result.is_ok())
            return read_result;

        read_result = kvdb::blockio::read_u64_t_le(
            *this->file,
            payload.seq_num,
            this->current_offset,
            BLOCK_SIZE
        );
        if (!read_result.is_ok())
            return read_result;

        const std::uint64_t remaining_payload =
            payload_end - this->current_offset;

        if (payload.key_size > remaining_payload ||
            payload.value_size > remaining_payload - payload.key_size)
        {
            return Status{
                StatusCode::Corruption,
                std::format(
                    "Invalid payload sizes: key_size={}, value_size={}, record_offset={}",
                    payload.key_size,
                    payload.value_size,
                    payload_record_start
                )
            };
        }

        auto key_alloc = this->arena.alloc(payload.key_size, alignof(std::byte));
        if (!key_alloc.is_ok())
            return std::move(key_alloc.status);

        auto value_alloc = this->arena.alloc(payload.value_size, alignof(std::byte));
        if (!value_alloc.is_ok())
            return std::move(value_alloc.status);

        payload.key_ptr = key_alloc.value;
        payload.value_ptr = value_alloc.value;

        read_result = kvdb::blockio::read_bytes(
            *this->file,
            reinterpret_cast<std::byte*>(payload.key_ptr),
            payload.key_size,
            this->current_offset,
            BLOCK_SIZE
        );
        if (!read_result.is_ok())
            return read_result;

        read_result = kvdb::blockio::read_bytes(
            *this->file,
            reinterpret_cast<std::byte*>(payload.value_ptr),
            payload.value_size,
            this->current_offset,
            BLOCK_SIZE
        );
        if (!read_result.is_ok())
            return read_result;

        data_block.payloads.emplace_back(std::move(payload));
    }

    if (this->current_offset != payload_end) {
        return Status{
            StatusCode::Corruption,
            "Payload parser did not stop exactly at payload_end"
        };
    }

    std::uint32_t actual_crc = 0;
    data_block.calculate_crc32(actual_crc);

    if (actual_crc != expected_crc) {
        return Status{
            StatusCode::ChecksumMismatch,
            std::format(
                "Data block CRC mismatch: expected={}, actual={}",
                expected_crc,
                actual_crc
            )
        };
    }

    for (auto& payload : data_block.payloads)
    {
        InternalRecord record{};
        record.key_entry = ArenaEntry(payload.key_ptr, payload.key_size);
        record.value_entry = ArenaEntry(payload.value_ptr, payload.value_size);

        record.type = payload.type;
        record.seq_num = payload.seq_num;

        this->current_block_records.emplace_back(record);
    }

    this->current_offset = block_end;
    next_block_index++;

    if (current_block_records.empty())
    {
        return Status
        {
            StatusCode::Corruption,
            "Loaded empty data block"
        };
    }

    valid_ = true;
    return Status::ok();
}

Status SSTableIterator::seek_to_first()
{
    current_offset = sstable.file_footer_section.data_offset;
    data_block_count = sstable.file_footer_section.data_block_count;
    next_block_index = 0;
    record_index = 0;
    valid_ = false;
    status_ = Status::ok();
    current_block_records.clear();

    status_ = load_next_block();
    return status_;
}