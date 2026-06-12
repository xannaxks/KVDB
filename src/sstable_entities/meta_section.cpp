#include "sstable_entities/meta_section.h"

using namespace SSTableEntities;

std::size_t MetaSection::Payload::disk_size()
{
    return (
        sizeof(record_count) +
        sizeof(tombstone_count) +
        sizeof(min_seq_num) +
        sizeof(max_seq_num) +
        sizeof(min_key_size) +
        sizeof(max_key_size) +
        sizeof(data_block_count) +
        sizeof(data_bytes) +
        this->min_key_size +
        this->max_key_size
        );
}

Status MetaSection::Header::write(WritableFile& file, std::uint64_t& offset)
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
Status MetaSection::Payload::write(WritableFile& file, std::uint64_t& offset)
{
    std::uint64_t old_offset;
    old_offset = offset;

    Result<std::uint64_t> get_position_result = file.current_position();
    if (!get_position_result.is_ok())
        return std::move(get_position_result.status);

    assert(get_position_result.value == offset);

    Status ensure_fits_result = ensure_fits_in_block(file, Payload::disk_size(), offset, BLOCK_SIZE);

    if (!ensure_fits_result.is_ok())
        return std::move(ensure_fits_result);

    assert(old_offset == offset);

    Status write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->record_count, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->tombstone_count, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->min_seq_num, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->max_seq_num, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->min_key_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->max_key_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->data_block_count, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->data_bytes, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    if (this->min_key_size > 0 && this->min_key_ptr == nullptr)
        return Status{
            StatusCode::InvariantViolation,
            "Failed to write meta section payload: min key size is greater than 0 but min key pointer is null"
    };

    if (this->max_key_size > 0 && this->max_key_ptr == nullptr)
        return Status{
            StatusCode::InvariantViolation,
            "Failed to write meta section payload: max key size is greater than 0 but max key pointer is null"
    };

    write_endian_result = kvdb::blockio::write_bytes(
        file,
        std::span<std::byte>(reinterpret_cast<std::byte*>(this->min_key_ptr), this->min_key_size),
        offset,
        BLOCK_SIZE
    );
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_bytes(
        file,
        std::span<std::byte>(reinterpret_cast<std::byte*>(this->max_key_ptr), this->max_key_size),
        offset,
        BLOCK_SIZE
    );
    if (!write_endian_result.is_ok())
        return write_endian_result;

    return Status::ok();
}
Status MetaSection::write(WritableFile& file, std::uint64_t& offset, std::uint64_t& meta_offset)
{
    Result<std::uint64_t> get_position_result = file.current_position();
    if (!get_position_result.is_ok())
        return std::move(get_position_result.status);

    assert(get_position_result.value == offset);

    Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);

    if (!align_to_block_result.is_ok())
        return std::move(align_to_block_result);

    Status write_result;
    meta_offset = offset;

    write_result = this->header.write(file, offset);
    if (!write_result.is_ok())
        return write_result;

    write_result = this->payload.write(file, offset);
    if (!write_result.is_ok())
        return write_result;

}

Result<MetaSection::Header> MetaSection::Header::load(
    ReadableFile& file,
    std::uint64_t& offset
)
{
    //assert(static_cast<std::uint64_t>(file.tellg()) == offset);
    Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);

    if (!align_to_block_result.is_ok())
        return Result<MetaSection::Header>::fail(std::move(align_to_block_result));

    Header result;
    std::uint8_t tmp_type;
    Status read_endian_result;

    read_endian_result = kvdb::blockio::read_u8_t(file, tmp_type, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Header>::fail(std::move(read_endian_result));

    result.type = static_cast<BlockType>(tmp_type);
    if (result.type != BlockType::Meta)
        return Result<Header>::fail(
            Status{
                StatusCode::InvalidBlockType,
                "Expected Meta block type in MetaSection"
            }
        );

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.payload_size, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Header>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.crc32, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Header>::fail(std::move(read_endian_result));

    return Result<Header>::ok(std::move(result));
}
Result<MetaSection::Payload> MetaSection::Payload::load(ReadableFile& file, std::uint64_t& offset, Arena& arena, MetaSection::Header& header)
{
    //assert(static_cast<std::uint64_t>(file.tellg()) == offset);

    uint64_t old_offset = offset;

    Status ensure_fits_result = ensure_fits_in_block(file, header.payload_size, offset, BLOCK_SIZE);

    if (!ensure_fits_result.is_ok())
        return Result<Payload>::fail(std::move(ensure_fits_result));

    assert(old_offset == offset);

    Payload result{};
    Status read_endian_result;

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.record_count, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.tombstone_count, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.min_seq_num, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.max_seq_num, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.min_key_size, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.max_key_size, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.data_block_count, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.data_bytes, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    const std::uint64_t expected_size = MetaSection::Payload::fixed_disk_size() + result.min_key_size + result.max_key_size;

    if (expected_size != header.payload_size)
        return Result<Payload>::fail(
            Status{
                StatusCode::InvariantViolation,
                "Meta section payload size mismatch during loading"
            }
        );

    if (expected_size > BLOCK_SIZE - MetaSection::Header::disk_size())
        return Result<Payload>::fail(
            Status{
                StatusCode::InvariantViolation,
                "Meta section payload size exceeds block size"
            }
        );

    Result<void*> arena_alloc_result;

    arena_alloc_result = arena.alloc(result.min_key_size, alignof(std::byte));
    if (!arena_alloc_result.is_ok())
        return Result<Payload>::fail(std::move(arena_alloc_result.status));
    void* min_key_ptr = arena_alloc_result.value;

    arena_alloc_result = arena.alloc(result.max_key_size, alignof(std::byte));
    if (!arena_alloc_result.is_ok())
        return Result<Payload>::fail(std::move(arena_alloc_result.status));
    void* max_key_ptr = arena_alloc_result.value;

    if (result.min_key_size > 0 && min_key_ptr == nullptr)
        return Result<Payload>::fail(
            Status{
                StatusCode::AllocationFailed,
                "Failed to allocate memory for min key"
            }
        );

    if (result.max_key_size > 0 && max_key_ptr == nullptr)
        return Result<Payload>::fail(
            Status{
                StatusCode::AllocationFailed,
                "Failed to allocate memory for max key"
            }
        );

    read_endian_result = kvdb::blockio::read_bytes(
        file,
        reinterpret_cast<std::byte*>(min_key_ptr),
        result.min_key_size,
        offset,
        BLOCK_SIZE
    );
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_bytes(
        file,
        reinterpret_cast<std::byte*>(max_key_ptr),
        result.max_key_size,
        offset,
        BLOCK_SIZE
    );
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    if (result.disk_size() != header.payload_size)
        return Result<Payload>::fail(
            Status{
                StatusCode::InvariantViolation,
                "Meta section payload size mismatch during loading"
            }
        );

    result.min_key_ptr = min_key_ptr;
    result.max_key_ptr = max_key_ptr;

    return Result<Payload>::ok(std::move(result));
}
Result<MetaSection> MetaSection::load(
    ReadableFile& file,
    std::uint64_t& offset,
    IndexSection& index_section,
    Arena& arena,
    const std::uint64_t& meta_offset
) {
    //assert(static_cast<std::uint64_t>(file.tellg()) == offset);

    if (meta_offset % BLOCK_SIZE != 0)
        return Result<MetaSection>::fail(
            Status{
                StatusCode::InvalidBlockAlignment,
                "Meta section offset is not aligned to block size"
            }
        );

    offset = meta_offset;

    MetaSection result{};
    std::uint64_t meta_block_offset = offset;

    auto header_res = MetaSection::Header::load(file, offset);
    if (!header_res.is_ok())
        return Result<MetaSection>::fail(std::move(header_res.status));
    result.header = std::move(header_res.value);

    auto payload_res = MetaSection::Payload::load(file, offset, arena, result.header);
    if (!payload_res.is_ok())
        return Result<MetaSection>::fail(std::move(payload_res.status));
    result.payload = std::move(payload_res.value);

    std::uint32_t must_be_crc32;
    result.calculate_crc32(must_be_crc32);

    if (must_be_crc32 != result.header.crc32)
        return Result<MetaSection>::fail(
            Status{
                StatusCode::ChecksumMismatch,
                std::format("SSTable header CRC mismatch: expected={}, actual={}",
                   must_be_crc32, result.header.crc32)
            }
        );

    if (result.header.payload_size != result.payload.disk_size())
        return Result<MetaSection>::fail(
            Status{
                StatusCode::InvariantViolation,
                "Meta section header payload size does not match actual payload size during loading"
            }
        );
    if (result.payload.data_block_count != index_section.payloads.size())
        return Result<MetaSection>::fail(
            Status{
                StatusCode::InvariantViolation,
                "Meta section data block count does not match index section data block count during loading"
            }
        );

    return Result<MetaSection>::ok(std::move(result));
}

void MetaSection::Payload::calculate_crc32(std::uint32_t& crc_buffer)
{
    crc_buffer = ::crc32(0L, Z_NULL, 0);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->record_count);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->tombstone_count);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->min_seq_num);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->max_seq_num);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->min_key_size);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->max_key_size);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->data_block_count);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->data_bytes);
    compute_crc32(crc_buffer, this->min_key_ptr, this->min_key_size);
    compute_crc32(crc_buffer, this->max_key_ptr, this->max_key_size);
}
void MetaSection::calculate_crc32(std::uint32_t& crc_buffer)
{
    this->payload.calculate_crc32(crc_buffer);
}

void MetaSection::rebuild(DataSection& data_section, IndexSection& index_section)
{
    this->payload.record_count = 0;
    this->payload.tombstone_count = 0;
    this->payload.min_seq_num = std::numeric_limits<std::uint64_t>::max();
    this->payload.max_seq_num = 0;
    this->payload.min_key_size = 0;
    this->payload.max_key_size = 0;
    this->payload.data_block_count = 0;
    this->payload.data_bytes = 0;

    this->payload.min_key_ptr = nullptr;
    this->payload.max_key_ptr = nullptr;

    this->payload.data_block_count = data_section.data_blocks.size();

    bool has_key = false;

    for (const auto& block : data_section.data_blocks)
    {
        this->payload.data_bytes += block.disk_size();
        for (const auto& payload : block.payloads)
        {
            ++this->payload.record_count;
            this->payload.tombstone_count += (payload.type == Type::Tombstone);
            this->payload.min_seq_num = std::min(this->payload.min_seq_num, payload.seq_num);
            this->payload.max_seq_num = std::max(this->payload.max_seq_num, payload.seq_num);

            ArenaEntry cur(payload.key_ptr, payload.key_size);

            if (!has_key)
            {
                this->payload.min_seq_num = payload.seq_num;
                this->payload.max_seq_num = payload.seq_num;

                this->payload.min_key_ptr = payload.key_ptr;
                this->payload.max_key_ptr = payload.key_ptr;
                this->payload.min_key_size = payload.key_size;
                this->payload.max_key_size = payload.key_size;

                has_key = true;

                continue;
            }

            ArenaEntry min_key(this->payload.min_key_ptr, this->payload.min_key_size);
            ArenaEntry max_key(this->payload.max_key_ptr, this->payload.max_key_size);

            if (cur < min_key)
            {
                this->payload.min_key_ptr = payload.key_ptr;
                this->payload.min_key_size = payload.key_size;
            }

            if (cur > max_key)
            {
                this->payload.max_key_ptr = payload.key_ptr;
                this->payload.max_key_size = payload.key_size;
            }
        }
    }

    if (!has_key)
    {
        this->payload.min_seq_num = 0;
        this->payload.max_seq_num = 0;
        this->payload.min_key_size = 0;
        this->payload.max_key_size = 0;

        this->payload.min_key_ptr = nullptr;
        this->payload.max_key_ptr = nullptr;
    }

    this->header.payload_size = this->payload.disk_size();
    this->calculate_crc32(this->header.crc32);
}

std::size_t MetaSection::Header::disk_size()
{
    return (
        sizeof(type) +
        sizeof(payload_size) +
        sizeof(crc32)
        );
}
std::size_t MetaSection::Payload::fixed_disk_size()
{
    return (
        sizeof(record_count) +
        sizeof(tombstone_count) +
        sizeof(min_seq_num) +
        sizeof(max_seq_num) +
        sizeof(min_key_size) +
        sizeof(max_key_size) +
        sizeof(data_block_count) +
        sizeof(data_bytes)
        );
}


std::size_t MetaSection::fixed_disk_size()
{
    return Header::fixed_disk_size() + Payload::fixed_disk_size();
}
std::size_t MetaSection::disk_size()
{
    return Header::disk_size() + this->payload.disk_size();
}
std::size_t MetaSection::Header::fixed_disk_size()
{
    return (
        sizeof(type) +
        sizeof(payload_size) +
        sizeof(crc32)
        );
}

MetaSection::MetaSection()
{
    header.type = BlockType::Meta;
    header.payload_size = Payload::fixed_disk_size();
    header.crc32 = ::crc32(0L, Z_NULL, 0);

    payload.record_count = 0;
    payload.tombstone_count = 0;
    payload.min_seq_num = 0;
    payload.max_seq_num = 0;
    payload.min_key_size = 0;
    payload.max_key_size = 0;
    payload.data_block_count = 0;
    payload.data_bytes = 0;

    crc32_add_pod<std::uint64_t>(header.crc32, payload.record_count);
    crc32_add_pod<std::uint64_t>(header.crc32, payload.tombstone_count);
    crc32_add_pod<std::uint64_t>(header.crc32, payload.min_seq_num);
    crc32_add_pod<std::uint64_t>(header.crc32, payload.max_seq_num);
    crc32_add_pod<std::uint32_t>(header.crc32, payload.min_key_size);
    crc32_add_pod<std::uint32_t>(header.crc32, payload.max_key_size);
    crc32_add_pod<std::uint64_t>(header.crc32, payload.data_block_count);
    crc32_add_pod<std::uint64_t>(header.crc32, payload.data_bytes);
}
