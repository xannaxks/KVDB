#include "sstable_entities/bloom_section.h"
#include "sstable_entities/index_section.h"
#include "sstable_entities/data_section.h"

using namespace SSTableEntities;

static std::uint64_t fnv1a64(const void* data, std::uint32_t size, std::uint64_t seed)
{
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint64_t h = 14695981039346656037ull ^ seed;

    for (std::uint32_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }

    return h;
}
static std::uint64_t bloom_hash_i(const void* key, std::uint32_t key_size, std::uint32_t i)
{
    std::uint64_t h1 = fnv1a64(key, key_size, 0x9E3779B97F4A7C15ull);
    std::uint64_t h2 = fnv1a64(key, key_size, 0xC2B2AE3D27D4EB4Full);

    return h1 + i * h2;
}
BloomSection::BloomSection()
{
    header.type = BlockType::Bloom;
    header.payload_size = Payload::disk_size();
    header.crc32 = ::crc32(0L, Z_NULL, 0);

    payload.bloom_bits = payload.mask.size();
    payload.hash_count = BLOOM_HASH_COUNT;
    payload.key_count = 0;
    payload.mask.resize(BLOOM_MASK_BIT_SIZE, 0);

    crc32_add_pod<std::uint64_t>(header.crc32, payload.bloom_bits);
    crc32_add_pod<std::uint32_t>(header.crc32, payload.hash_count);
    crc32_add_pod<std::uint32_t>(header.crc32, payload.key_count);

    compute_crc32(header.crc32, payload.mask.data(), payload.mask.size());
}

Result<BloomSection::Header> BloomSection::Header::load(ReadableFile& file, std::uint64_t& offset)
{
    //assert(static_cast<std::uint64_t>(file.tellg()) == offset);

    Header result{};
    std::uint8_t tmp_type;
    Status read_endian_result;

    read_endian_result = std::move(kvdb::blockio::read_u8_t(file, tmp_type, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<Header>::fail(std::move(read_endian_result));

    result.type = static_cast<BlockType>(tmp_type);
    if (result.type != BlockType::Bloom)
        return Result<Header>::fail(Status{
            StatusCode::InvariantViolation,
            "Invalid block type for BloomSection header"
            });

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.payload_size, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Header>::fail(std::move(read_endian_result));

    if (result.payload_size != BloomSection::Payload::disk_size())
        return Result<Header>::fail(Status{
            StatusCode::InvariantViolation,
            "Invalid payload size for BloomSection header"
            });

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.crc32, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Header>::fail(std::move(read_endian_result));

    return Result<Header>::ok(std::move(result));
}
Result<BloomSection::Payload> BloomSection::Payload::load(ReadableFile& file, std::uint64_t& offset)
{
    //assert(static_cast<std::uint64_t>(file.tellg()) == offset);

    Payload result{};
    std::uint64_t payload_size = 0;
    Status read_endian_result;

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.bloom_bits, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.hash_count, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.key_count, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    result.mask.resize(BLOOM_MASK_BIT_SIZE);

    read_endian_result = kvdb::blockio::read_bytes(
        file,
        reinterpret_cast<std::byte*>(result.mask.data()),
        BLOOM_MASK_BIT_SIZE,
        offset,
        BLOCK_SIZE
    );
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    return Result<Payload>::ok(std::move(result));
}
Result<BloomSection> BloomSection::load(ReadableFile& file, std::uint64_t& offset, const std::uint64_t& bloom_offset)
{
    //assert(static_cast<std::uint64_t>(file.tellg()) == offset);

    if (bloom_offset % BLOCK_SIZE != 0)
        return Result<BloomSection>::fail(
            Status{
                StatusCode::InvariantViolation,
                "Bloom section offset is not aligned to block boundary during loading"
            }
        );

    offset = bloom_offset;

    BloomSection result{};
    std::uint64_t bloom_block_offset = offset;

    auto header_res = BloomSection::Header::load(file, offset);
    if (!header_res.is_ok())
        return Result<BloomSection>::fail(std::move(header_res.status));
    result.header = std::move(header_res.value);

    auto payload_res = BloomSection::Payload::load(file, offset);
    if (!payload_res.is_ok())
        return Result<BloomSection>::fail(std::move(payload_res.status));
    result.payload = std::move(payload_res.value);

    std::uint32_t must_be_crc32;
    result.calculate_crc32(must_be_crc32);

    if (must_be_crc32 != result.header.crc32)
        return Result<BloomSection>::fail(
            Status{
                StatusCode::ChecksumMismatch,
                std::format("SSTable header CRC mismatch: expected={}, actual={}",
                           must_be_crc32, result.header.crc32)
            }
        );
    if (result.payload.hash_count != BLOOM_HASH_COUNT) return Result<BloomSection>::fail(
        Status{ StatusCode::InvariantViolation, "Bloom section hash count mismatch" });
    if (result.payload.disk_size() != result.header.payload_size) return Result<BloomSection>::fail(
        Status{ StatusCode::InvariantViolation, "Bloom section payload size mismatch" });
    if (result.payload.bloom_bits != result.payload.mask.size()) return Result<BloomSection>::fail(
        Status{ StatusCode::InvariantViolation, "Bloom section bloom bits mismatch" });

    return Result<BloomSection>::ok(std::move(result));
}

void BloomSection::Payload::calculate_crc32(std::uint32_t& crc_buffer)
{
    crc_buffer = ::crc32(0L, Z_NULL, 0);

    crc32_add_pod<std::uint64_t>(crc_buffer, this->bloom_bits);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->hash_count);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->key_count);

    compute_crc32(crc_buffer, this->mask.data(), this->mask.size());
}
void BloomSection::calculate_crc32(std::uint32_t& crc_buffer)
{
    this->payload.calculate_crc32(crc_buffer);
}

Status BloomSection::Header::write(WritableFile& file, std::uint64_t& offset)
{
    Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);

    if (!align_to_block_result.is_ok())
        return std::move(align_to_block_result);

    assert(offset / BLOCK_SIZE == (offset + this->disk_size() - 1) / BLOCK_SIZE);

    Status write_endian_result;

    write_endian_result = kvdb::blockio::write_u8_t(file, static_cast<uint8_t>(this->type), offset, BLOCK_SIZE);
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
Status BloomSection::Payload::write(WritableFile& file, std::uint64_t& offset)
{
    std::uint64_t old_offset = offset;
    Status ensure_fits_result = ensure_fits_in_block(file, Payload::disk_size(), offset, BLOCK_SIZE);
    if (!ensure_fits_result.is_ok())
        return std::move(ensure_fits_result);

    assert(old_offset == offset);
    assert(offset / BLOCK_SIZE == (offset + this->disk_size() - 1) / BLOCK_SIZE);

    Status write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->bloom_bits, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->hash_count, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->key_count, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_bytes(
        file,
        std::span<std::byte>(reinterpret_cast<std::byte*>(this->mask.data()), this->mask.size()),
        offset,
        BLOCK_SIZE
    );
    if (!write_endian_result.is_ok())
        return write_endian_result;

    return Status::ok();
}
Status BloomSection::write(WritableFile& file, std::uint64_t& offset, std::uint64_t& bloom_offset)
{
    Result<std::uint64_t> get_position_result = file.current_position();
    if (!get_position_result.is_ok())
        return std::move(get_position_result.status);

    assert(get_position_result.value == offset);

    Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);
    if (!align_to_block_result.is_ok())
        return std::move(align_to_block_result);

    Status write_result;
    bloom_offset = offset;

    write_result = this->header.write(file, offset);
    if (!write_result.is_ok())
        return write_result;

    write_result = this->payload.write(file, offset);
    if (!write_result.is_ok())
        return write_result;

    return Status::ok();
}

void BloomSection::add_key(const void* key_ptr, std::uint32_t key_size)
{
    if (!key_ptr || key_size == 0)
        return;

    for (std::uint32_t i = 0; i < payload.hash_count; ++i) {
        std::uint64_t h = bloom_hash_i(key_ptr, key_size, i);
        std::uint64_t bit = h % payload.bloom_bits;
        payload.mask[bit] = 1;
    }

    ++payload.key_count;
}
bool BloomSection::may_contain(const void* key_ptr, std::uint32_t key_size) const
{
    if (!key_ptr || !key_size) return false;

    for (std::size_t i = 0; i < payload.hash_count; i++)
    {
        std::uint64_t h = bloom_hash_i(key_ptr, key_size, i);
        std::uint64_t bit = h % payload.bloom_bits;

        if (!payload.mask[bit])
            return false;
    }

    return true;
}
void BloomSection::recompute_crc32()
{
    header.type = BlockType::Bloom;
    header.payload_size = Payload::disk_size();
    header.crc32 = ::crc32(0L, Z_NULL, 0);

    payload.calculate_crc32(header.crc32);
}
void BloomSection::rebuild(const DataSection& data_section)
{
    payload.bloom_bits = static_cast<std::uint32_t>(payload.mask.size());
    payload.hash_count = BLOOM_HASH_COUNT;
    payload.key_count = 0;
    payload.mask.assign(payload.mask.size(), 0);

    for (const auto& block : data_section.data_blocks)
    {
        for (const auto& data_block_payload : block.payloads)
        {
            add_key(data_block_payload.key_ptr, data_block_payload.key_size);
        }
    }

    recompute_crc32();
}

std::size_t BloomSection::disk_size()
{
	return Header::disk_size() + Payload::disk_size();
}