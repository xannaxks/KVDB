#include "sstable_entities/index_section.h"
#include <cassert>

using namespace SSTableEntities;

std::size_t IndexSection::Header::disk_size()
{
    return (
        sizeof(type) +
        sizeof(payload_size) +
        sizeof(crc32)
        );
}
std::size_t IndexSection::Payload::disk_size()
{
    return (
        sizeof(first_key_size) +
        sizeof(last_key_size) +
        first_key_size +
        last_key_size +
        sizeof(data_block_offset)
        );
}
std::size_t IndexSection::Payload::fixed_disk_size()
{
    return (
        sizeof(data_block_offset) +
        sizeof(first_key_size) +
        sizeof(last_key_size)
        );
}
std::size_t IndexSection::disk_size()
{
    std::size_t cnt = 0;
    for (auto& i : payloads)
        cnt += i.disk_size();
    return (
        Header::disk_size() + cnt
        );
}
IndexSection::IndexSection()
{
    header.type = BlockType::Index;
    header.payload_size = 0;
    header.crc32 = ::crc32(0L, Z_NULL, 0);
}


Result<IndexSection::Header> IndexSection::Header::load(ReadableFile& file, uint64_t& offset)
{
    Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);

    if (!align_to_block_result.is_ok())
        return Result<IndexSection::Header>::fail(std::move(align_to_block_result));

    Header result{};
    uint8_t tmp_type;
    Status read_endian_result;

    read_endian_result = std::move(kvdb::blockio::read_u8_t(file, tmp_type, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<IndexSection::Header>::fail(std::move(read_endian_result));

    result.type = static_cast<BlockType>(tmp_type);
    if (result.type != BlockType::Index)
        return Result<IndexSection::Header>::fail(
            Status{
                StatusCode::InvalidBlockType,
                "Expected Index block type in IndexSection"
            }
        );

    read_endian_result = std::move(kvdb::blockio::read_u32_t_le(file, result.payload_size, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<IndexSection::Header>::fail(std::move(read_endian_result));

    if (result.payload_size > BLOCK_SIZE - Header::disk_size())
        return Result<Header>::fail(Status{
            StatusCode::InvalidPayloadSize,
            "Index section payload exceeds its single-block capacity"
        });

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.crc32, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<IndexSection::Header>::fail(std::move(read_endian_result));

    return Result<IndexSection::Header>::ok(std::move(result));
}

Result<IndexSection::Payload> IndexSection::Payload::load(
    ReadableFile& file,
    std::uint64_t& offset,
    Arena& arena
)
{
    Status ensure_fits_result = ensure_fits_in_block(
        file,
        IndexSection::Payload::fixed_disk_size(),
        offset,
        BLOCK_SIZE
    );
    if (!ensure_fits_result.is_ok())
        return Result<Payload>::fail(std::move(ensure_fits_result));

    const Arena::Checkpoint checkpoint = arena.checkpoint();
    Payload result{};
    Status status;

    status = kvdb::blockio::read_u64_t_le(
        file,
        result.data_block_offset,
        offset,
        BLOCK_SIZE
    );
    if (!status.is_ok())
        return Result<Payload>::fail(std::move(status));

    if (result.data_block_offset % BLOCK_SIZE != 0)
        return Result<Payload>::fail(Status{
            StatusCode::InvalidSectionOffset,
            "Index entry points to an unaligned data block"
        });

    status = kvdb::blockio::read_u32_t_le(
        file,
        result.first_key_size,
        offset,
        BLOCK_SIZE
    );
    if (!status.is_ok())
        return Result<Payload>::fail(std::move(status));

    status = kvdb::blockio::read_u32_t_le(
        file,
        result.last_key_size,
        offset,
        BLOCK_SIZE
    );
    if (!status.is_ok())
        return Result<Payload>::fail(std::move(status));

    const std::uint64_t key_bytes =
        static_cast<std::uint64_t>(result.first_key_size) +
        static_cast<std::uint64_t>(result.last_key_size);
    if (key_bytes > BLOCK_SIZE - Payload::fixed_disk_size())
        return Result<Payload>::fail(Status{
            StatusCode::InvalidPayloadSize,
            "Index entry keys exceed the remaining block capacity"
        });

    ensure_fits_result = ensure_fits_in_block(
        file,
        static_cast<std::size_t>(key_bytes),
        offset,
        BLOCK_SIZE
    );
    if (!ensure_fits_result.is_ok())
        return Result<Payload>::fail(std::move(ensure_fits_result));

    void* first_key_ptr = nullptr;
    if (result.first_key_size > 0) {
        Result<void*> allocation = arena.alloc(
            result.first_key_size,
            alignof(std::byte)
        );
        if (!allocation.is_ok()) {
            arena.rollback(checkpoint);
            return Result<Payload>::fail(std::move(allocation.status));
        }
        first_key_ptr = allocation.value;
    }

    void* last_key_ptr = nullptr;
    if (result.last_key_size > 0) {
        Result<void*> allocation = arena.alloc(
            result.last_key_size,
            alignof(std::byte)
        );
        if (!allocation.is_ok()) {
            arena.rollback(checkpoint);
            return Result<Payload>::fail(std::move(allocation.status));
        }
        last_key_ptr = allocation.value;
    }

    if (result.first_key_size > 0) {
        status = kvdb::blockio::read_bytes(
            file,
            reinterpret_cast<std::byte*>(first_key_ptr),
            result.first_key_size,
            offset,
            BLOCK_SIZE
        );
        if (!status.is_ok()) {
            arena.rollback(checkpoint);
            return Result<Payload>::fail(std::move(status));
        }
    }

    if (result.last_key_size > 0) {
        status = kvdb::blockio::read_bytes(
            file,
            reinterpret_cast<std::byte*>(last_key_ptr),
            result.last_key_size,
            offset,
            BLOCK_SIZE
        );
        if (!status.is_ok()) {
            arena.rollback(checkpoint);
            return Result<Payload>::fail(std::move(status));
        }
    }

    result.first_key_ptr = first_key_ptr;
    result.last_key_ptr = last_key_ptr;
    return Result<Payload>::ok(std::move(result));
}
Result<IndexSection> IndexSection::load(
    ReadableFile& file,
    std::uint64_t& offset,
    Arena& arena,
    const std::uint64_t& index_offset
)
{
    //assert(file. == offset);

    if (index_offset % BLOCK_SIZE != 0)
        return Result<IndexSection>::fail(
            Status{
                StatusCode::InvalidAlignment,
                "Index section not alinged to block boundary"
            }
        );

    offset = index_offset;

    IndexSection result{};
    std::uint64_t index_block_offset = offset;

    auto header_res = IndexSection::Header::load(file, offset);
    if (!header_res.is_ok())
        return Result<IndexSection>::fail(std::move(header_res.status));
    result.header = std::move(header_res.value);

    // payload

    std::uint64_t payload_bytes_read = 0;

    while (payload_bytes_read < result.header.payload_size)
    {
        auto payload_res = IndexSection::Payload::load(file, offset, arena);
        if (!payload_res.is_ok())
            return Result<IndexSection>::fail(std::move(payload_res.status));

        if (payload_res.value.disk_size() > result.header.payload_size - payload_bytes_read)
            return Result<IndexSection>::fail(
                Status{
                    StatusCode::InvariantViolation,
                    "Payload size exceeds remaining index section payload size"
                }
            );
        payload_bytes_read += payload_res.value.disk_size();
        result.payloads.emplace_back(std::move(payload_res.value));

    }

    if (payload_bytes_read != result.header.payload_size)
        return Result<IndexSection>::fail(
            Status{
                StatusCode::InvariantViolation,
                "Payload size does not match index section payload size"
            }
        );


    std::uint32_t must_be_crc32;
    result.calculate_crc32(must_be_crc32);

    if (must_be_crc32 != result.header.crc32)
        return Result<IndexSection>::fail(
            Status{
                StatusCode::ChecksumMismatch,
                std::format("SSTable header CRC mismatch: expected={}, actual={}",
                           must_be_crc32, result.header.crc32)
            }
        );

    return Result<IndexSection>::ok(std::move(result));
}

void IndexSection::Payload::calculate_crc32(std::uint32_t& crc_buffer)
{
    crc_buffer = ::crc32(0L, Z_NULL, 0);

    this->append_crc32(crc_buffer);
}
void IndexSection::Payload::append_crc32(std::uint32_t& crc_buffer)
{
    crc32_add_pod<std::uint64_t>(crc_buffer, this->data_block_offset);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->first_key_size);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->last_key_size);

    if (this->first_key_size > 0)
        compute_crc32(crc_buffer, this->first_key_ptr, this->first_key_size);
    if (this->last_key_size > 0)
        compute_crc32(crc_buffer, this->last_key_ptr, this->last_key_size);
}
void IndexSection::calculate_crc32(std::uint32_t& crc_buffer)
{
    crc_buffer = ::crc32(0L, Z_NULL, 0);
    for (IndexSection::Payload& payload : this->payloads)
        payload.append_crc32(crc_buffer);
}

Status IndexSection::write(WritableFile& file, std::uint64_t& offset, std::uint64_t& index_offset)
{
    Result<std::uint64_t> get_position_result = file.current_position();
    if (!get_position_result.is_ok())
        return std::move(get_position_result.status);

    assert(get_position_result.value == offset);

    Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);

    if (!align_to_block_result.is_ok())
        return std::move(align_to_block_result);

    Status write_result;
    index_offset = offset;

    write_result = this->header.write(file, offset);
    if (!write_result.is_ok())
        return write_result;

    for (auto& payload : this->payloads)
    {
        write_result = payload.write(file, offset);
        if (!write_result.is_ok())
            return write_result;
    }

    return Status::ok();
}
Status IndexSection::Header::write(WritableFile& file, std::uint64_t& offset)
{
    Result<std::uint64_t> get_position_result = file.current_position();
    if (!get_position_result.is_ok())
        return std::move(get_position_result.status);

    assert(get_position_result.value == offset);

    Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);

    if (!align_to_block_result.is_ok())
        return std::move(align_to_block_result);

    assert(offset / BLOCK_SIZE == (offset + this->disk_size() - 1) / BLOCK_SIZE);

    Status write_endian_result;

    write_endian_result = kvdb::blockio::write_u8_t(file, static_cast<std::uint8_t>(this->type), offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->payload_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->crc32, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    return Status::ok();
}
Status IndexSection::Payload::write(WritableFile& file, std::uint64_t& offset)
{
    Result<std::uint64_t> get_position_result = file.current_position();
    if (!get_position_result.is_ok())
        return std::move(get_position_result.status);

    assert(get_position_result.value == offset);

    if (data_block_offset % BLOCK_SIZE != 0)
        return Status{ StatusCode::InvalidSectionOffset, "Index entry data offset is not block aligned" };
    if (first_key_size > 0 && first_key_ptr == nullptr)
        return Status{ StatusCode::NullPointer, "Index first-key pointer is null" };
    if (last_key_size > 0 && last_key_ptr == nullptr)
        return Status{ StatusCode::NullPointer, "Index last-key pointer is null" };

    Status ensure_fits_result = ensure_fits_in_block(file, Payload::fixed_disk_size(), offset, BLOCK_SIZE);
    if (!ensure_fits_result.is_ok())
        return std::move(ensure_fits_result);

    Status write_endian_result;

    write_endian_result = std::move(kvdb::blockio::write_u64_t_le(file, this->data_block_offset, offset, BLOCK_SIZE));
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = std::move(kvdb::blockio::write_u32_t_le(file, this->first_key_size, offset, BLOCK_SIZE));
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->last_key_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    ensure_fits_result = ensure_fits_in_block(file, this->first_key_size, offset, BLOCK_SIZE);
    if (!ensure_fits_result.is_ok())
        return std::move(ensure_fits_result);

    write_endian_result = kvdb::blockio::write_bytes(
        file,
        std::span<std::byte>(reinterpret_cast<std::byte*>(this->first_key_ptr), this->first_key_size),
        offset,
        BLOCK_SIZE
    );
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_bytes(
        file,
        std::span<std::byte>(reinterpret_cast<std::byte*>(this->last_key_ptr), this->last_key_size),
        offset,
        BLOCK_SIZE
    );
    if (!write_endian_result.is_ok())
        return write_endian_result;

    return Status::ok();
}

void IndexSection::add_index(
    std::uint64_t data_block_offset,
    std::uint32_t first_key_size,
    std::uint32_t last_key_size,
    void* first_key_ptr,
    void* last_key_ptr
)
{
    Payload payload{};
    payload.data_block_offset = data_block_offset;
    payload.first_key_size = first_key_size;
    payload.last_key_size = last_key_size;
    payload.first_key_ptr = first_key_ptr;
    payload.last_key_ptr = last_key_ptr;

    this->payloads.emplace_back(payload);

    this->header.payload_size +=
        sizeof(payload.data_block_offset) +
        sizeof(payload.first_key_size) +
        sizeof(payload.last_key_size) +
        payload.first_key_size +
        payload.last_key_size;

    crc32_add_pod<std::uint64_t>(this->header.crc32, payload.data_block_offset);
    crc32_add_pod<std::uint32_t>(this->header.crc32, payload.first_key_size);
    crc32_add_pod<std::uint32_t>(this->header.crc32, payload.last_key_size);

    if (payload.first_key_ptr && payload.first_key_size > 0)
        compute_crc32(this->header.crc32, payload.first_key_ptr, payload.first_key_size);

    if (payload.last_key_ptr && payload.last_key_size > 0)
        compute_crc32(this->header.crc32, payload.last_key_ptr, payload.last_key_size);
}


std::size_t IndexSection::fixed_disk_size()
{
	return (
		Header::disk_size() +
		Payload::fixed_disk_size()
		);
}
