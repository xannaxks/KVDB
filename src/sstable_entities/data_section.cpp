#include "sstable_entities/data_section.h"

using namespace SSTableEntities;

bool DataSection::DataBlock::can_payload_fit(DataSection::Payload& payload)
{
    return this->disk_size() + payload.disk_size() <= BLOCK_SIZE;
}


/// init new block


void DataSection::init_new_block()
{
    data_blocks.emplace_back(DataSection::Header());
}


/// disk_size

std::size_t DataSection::Header::disk_size()
{
    return (
        sizeof(type) +
        sizeof(payload_disk_size) +
        sizeof(crc32)
        );
}
std::size_t DataSection::Payload::disk_size()
{
    return (
        sizeof(key_size) +
        sizeof(value_size) +
        key_size +
        value_size +
        sizeof(type) +
        sizeof(flags) +
        sizeof(reserved) +
        sizeof(seq_num)
        );
}
std::size_t DataSection::Payload::disk_size() const
{
    return (
        sizeof(key_size) +
        sizeof(value_size) +
        key_size +
        value_size +
        sizeof(type) +
        sizeof(flags) +
        sizeof(reserved) +
        sizeof(seq_num)
        );
}


DataSection::Payload::Payload(const InternalRecord& record)
    : key_size(record.key_entry.size),
    value_size(record.value_entry.size),
    type(record.type),
    seq_num(record.seq_num),
    reserved(0),
    flags(0),
    key_ptr(record.key_entry.data),
    value_ptr(record.value_entry.data)
{
}

std::size_t DataSection::Payload::fixed_part_disk_size()
{
    return sizeof(key_size) + sizeof(value_size) + sizeof(type) + sizeof(flags) + sizeof(reserved) + sizeof(seq_num);
}
std::size_t DataSection::disk_size()
{
    std::size_t cnt = 0;
    for (auto& block : data_blocks)
        cnt += block.disk_size();
    return cnt;
}
std::size_t DataSection::DataBlock::disk_size()
{
    std::size_t res = Header::disk_size();
    for (auto& payload : this->payloads)
        res += payload.disk_size();
    return res;
}
std::size_t DataSection::DataBlock::disk_size() const
{
    std::size_t res = Header::disk_size();
    for (auto& payload : this->payloads)
        res += payload.disk_size();
    return res;
}



DataSection::Header::Header()
    : type(BlockType::Data), payload_disk_size(0), crc32(::crc32(0L, Z_NULL, 0))
{
}


void DataSection::DataBlock::add_payload(Payload& payload)
{
    header.payload_disk_size += payload.disk_size();

    // CRC of logical serialized content
    crc32_add_pod<std::uint32_t>(header.crc32, payload.key_size);
    crc32_add_pod<std::uint32_t>(header.crc32, payload.value_size);
    crc32_add_pod<std::uint8_t>(header.crc32, static_cast<std::uint8_t>(payload.type));
    crc32_add_pod<std::uint32_t>(header.crc32, payload.flags);
    crc32_add_pod<std::uint32_t>(header.crc32, payload.reserved);
    crc32_add_pod<std::uint64_t>(header.crc32, payload.seq_num);

    if (payload.key_ptr && payload.key_size > 0)
        compute_crc32(header.crc32, payload.key_ptr, payload.key_size);

    if (payload.value_ptr && payload.value_size > 0)
        compute_crc32(header.crc32, payload.value_ptr, payload.value_size);

    payloads.emplace_back(payload);
}
Status DataSection::add_payload(const InternalRecord& record)
{
    Payload payload{};
    payload.key_size = static_cast<std::uint32_t>(record.key_entry.size);
    payload.value_size = static_cast<std::uint32_t>(record.value_entry.size);
    payload.type = record.type;
    payload.flags = 0;
    payload.reserved = 0;
    payload.seq_num = record.seq_num;
    payload.key_ptr = record.key_entry.data;
    payload.value_ptr = record.value_entry.data;

    if (payload.disk_size() > BLOCK_SIZE - DataSection::Header::disk_size()) {
        return Status{
            StatusCode::InvalidPayloadSize,
            std::format(
                "data block payload exceeds block capacity: payload_size={}, capacity={}",
                payload.disk_size(),
                BLOCK_SIZE - DataSection::Header::disk_size()
            )
        };
    }

    if (data_blocks.empty())
        init_new_block();

    if (!data_blocks.back().can_payload_fit(payload))
        init_new_block();

    data_blocks.back().add_payload(payload);

    return Status::ok();
}

Status DataSection::Header::write(WritableFile& file, std::uint64_t& offset)
{
    Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);
    if (!align_to_block_result.is_ok())
        return std::move(align_to_block_result);

    assert(offset / BLOCK_SIZE == (offset + this->disk_size() - 1) / BLOCK_SIZE);

    Status write_endian_result;

    write_endian_result = kvdb::blockio::write_u8_t(
        file,
        static_cast<std::uint8_t>(this->type),
        offset,
        BLOCK_SIZE
    );
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(
        file,
        this->payload_disk_size,
        offset,
        BLOCK_SIZE
    );
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(
        file,
        this->crc32,
        offset,
        BLOCK_SIZE
    );
    if (!write_endian_result.is_ok())
        return write_endian_result;

    return Status::ok();
}

Status DataSection::Payload::write(WritableFile& file, std::uint64_t& offset)
{
    Result<std::uint64_t> get_position_result = file.current_position();
    if (!get_position_result.is_ok())
        return std::move(get_position_result.status);

    assert(get_position_result.value == offset);
    assert(offset / BLOCK_SIZE == (offset + this->disk_size() - 1) / BLOCK_SIZE);

    if (this->disk_size() > BLOCK_SIZE - Header::disk_size())
        return Status{
            StatusCode::InvalidPayloadSize,
            std::format(
                "data block payload exceeds block capacity during writing: payload_size={}, capacity={}",
                this->disk_size(),
                BLOCK_SIZE - DataSection::Header::disk_size()
            )
    };

    Status write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->key_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok()) // notice : block alignment handled by write_type_t_le()
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->value_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u8_t(file, static_cast<std::uint8_t>(this->type), offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->flags, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->reserved, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->seq_num, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_bytes(
        file,
        std::span<std::byte>(reinterpret_cast<std::byte*>(this->key_ptr), this->key_size),
        offset,
        BLOCK_SIZE
    );
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_bytes(
        file,
        std::span<std::byte>(reinterpret_cast<std::byte*>(this->value_ptr), this->value_size),
        offset,
        BLOCK_SIZE
    );
    if (!write_endian_result.is_ok())
        return write_endian_result;

    return Status::ok();
}

Status DataSection::DataBlock::write(WritableFile& file, std::uint64_t& offset, IndexSection& index_section) {
    //assert(static_cast<std::uint64_t>(file.tellp()) == offset);
    Result<std::uint64_t> get_position_result = file.current_position();
    if (!get_position_result.is_ok())
        return std::move(get_position_result.status);

    assert(get_position_result.value == offset);

    Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);
    if (!align_to_block_result.is_ok())
        return std::move(align_to_block_result);

    // each datablock less or equal to the size of physical block size. it was adjusted during .add;

    Status write_result;
    std::uint64_t data_block_offset = offset;

    write_result = this->header.write(file, offset);
    if (!write_result.is_ok())
        return write_result;

    bool first_key_set = false;
    std::uint32_t first_key_size = 0, last_key_size = 0;
    void* first_key_ptr = nullptr;
    void* last_key_ptr = nullptr;

    for (auto& payload : this->payloads)
    {
        write_result = payload.write(file, offset);
        if (!write_result.is_ok())
            return write_result;

        //std::uint64_t key_offset = offset;

        if (!first_key_set)
        {
            first_key_ptr = payload.key_ptr;
            first_key_size = payload.key_size;
            first_key_set = true;
        }

        last_key_ptr = payload.key_ptr;
        last_key_size = payload.key_size;
    }

    if (!payloads.empty())
        index_section.add_index(data_block_offset, first_key_size, last_key_size, first_key_ptr, last_key_ptr);

    return Status::ok();
}
Status DataSection::write(WritableFile& file, std::uint64_t& offset, IndexSection& index_section, std::uint64_t& data_offset)
{
    Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);
    if (!align_to_block_result.is_ok())
        return std::move(align_to_block_result);

    Status write_result;
    data_offset = offset;
    for (auto& data_block : this->data_blocks)
    {
        write_result = data_block.write(file, offset, index_section);
        if (!write_result.is_ok())
            return write_result;
    }
    return Status::ok();
}


void DataSection::Payload::append_crc32(std::uint32_t& crc_buffer)
{
    crc32_add_pod<std::uint32_t>(crc_buffer, this->key_size);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->value_size);
    crc32_add_pod<std::uint8_t>(crc_buffer, static_cast<std::uint8_t>(this->type));
    crc32_add_pod<std::uint32_t>(crc_buffer, this->flags);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->reserved);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->seq_num);

    compute_crc32(crc_buffer, this->key_ptr, this->key_size);
    compute_crc32(crc_buffer, this->value_ptr, this->value_size);
}
void DataSection::Payload::calculate_crc32(std::uint32_t& crc_buffer)
{
    crc_buffer = ::crc32(0L, Z_NULL, 0);

    crc32_add_pod<std::uint32_t>(crc_buffer, this->key_size);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->value_size);
    crc32_add_pod<std::uint8_t>(crc_buffer, static_cast<std::uint8_t>(this->type));
    crc32_add_pod<std::uint32_t>(crc_buffer, this->flags);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->reserved);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->seq_num);

    compute_crc32(crc_buffer, this->key_ptr, this->key_size);
    compute_crc32(crc_buffer, this->value_ptr, this->value_size);
}
void DataSection::DataBlock::calculate_crc32(std::uint32_t& crc_buffer)
{
    crc_buffer = ::crc32(0L, Z_NULL, 0);
    for (DataSection::Payload& payload : this->payloads)
    {
        payload.append_crc32(crc_buffer);
    }
}
