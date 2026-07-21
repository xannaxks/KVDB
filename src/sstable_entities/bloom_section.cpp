#define NOMINMAX
#include "sstable_entities/bloom_section.h"

#include <algorithm>
#include <limits>
#include <span>
#include <string>
#include <utility>

#include "crc32_helpers.h"
#include "endian_io.h"
#include "file_helpers.h"
#include "sstable_entities/data_section.h"

namespace
{
    using SSTableEntities::BLOOM_HASH_COUNT;
    using SSTableEntities::BLOOM_MASK_BIT_SIZE;
    using SSTableEntities::BLOCK_SIZE;

    [[nodiscard]] std::uint64_t fnv1a64(
        const void* data,
        std::uint32_t size,
        std::uint64_t seed
    ) noexcept
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        std::uint64_t hash = 14695981039346656037ull ^ seed;

        for (std::uint32_t i = 0; i < size; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ull;
        }

        return hash;
    }

    [[nodiscard]] std::uint64_t bloom_hash_i(
        const void* key,
        std::uint32_t key_size,
        std::uint32_t index
    ) noexcept
    {
        const std::uint64_t first = fnv1a64(
            key,
            key_size,
            0x9E3779B97F4A7C15ull
        );
        const std::uint64_t second = fnv1a64(
            key,
            key_size,
            0xC2B2AE3D27D4EB4Full
        );

        return first + static_cast<std::uint64_t>(index) * second;
    }

    [[nodiscard]] Status validate_mask_values(
        const std::array<std::uint8_t, BLOOM_MASK_BIT_SIZE>& mask
    )
    {
        const auto invalid = std::find_if(
            mask.begin(),
            mask.end(),
            [](std::uint8_t value) { return value > 1u; }
        );

        if (invalid != mask.end()) {
            return Status{
                StatusCode::InvalidArgument,
                "Bloom mask contains a non-Boolean slot value"
            };
        }

        return Status::ok();
    }

    [[nodiscard]] Status validate_cursor(
        WritableFile& file,
        std::uint64_t offset
    )
    {
        Result<std::uint64_t> position = file.current_position();
        if (!position.is_ok()) {
            return std::move(position.status);
        }

        if (position.value != offset) {
            return Status{
                StatusCode::InvalidOffset,
                "tracked Bloom-section offset differs from the writable file cursor"
            };
        }

        return Status::ok();
    }

    [[nodiscard]] Status add_key_to_payload(
        SSTableEntities::BloomSection::Payload& payload,
        const void* key_ptr,
        std::uint32_t key_size
    )
    {
        if (key_size > 0 && key_ptr == nullptr) {
            return Status{
                StatusCode::NullPointer,
                "Bloom key pointer is null for a non-empty key"
            };
        }

        Status status = payload.validate();
        if (!status.is_ok()) {
            return Status{
                StatusCode::InvalidState,
                "cannot add a key to an invalid Bloom payload: " + status.message
            };
        }

        if (payload.key_count == std::numeric_limits<std::uint32_t>::max()) {
            return Status{
                StatusCode::DataTypeOverflow,
                "Bloom key count would overflow uint32_t"
            };
        }

        for (std::uint32_t i = 0; i < payload.hash_count; ++i) {
            const std::uint64_t hash = bloom_hash_i(key_ptr, key_size, i);
            const std::uint64_t slot = hash % payload.bloom_bits;
            payload.mask[static_cast<std::size_t>(slot)] = 1u;
        }

        ++payload.key_count;
        return Status::ok();
    }
}

namespace SSTableEntities
{
    Status BloomSection::Header::validate() const
    {
        if (type != BlockType::Bloom) {
            return Status{
                StatusCode::InvalidBlockType,
                "Bloom header has an invalid block type"
            };
        }

        if (payload_size != Payload::disk_size()) {
            return Status{
                StatusCode::InvalidPayloadSize,
                "Bloom header payload size does not match the version-1 format"
            };
        }

        return Status::ok();
    }

    Status BloomSection::Payload::validate() const
    {
        if (mask.size() != BLOOM_MASK_BIT_SIZE) {
            return Status{
                StatusCode::InvalidPayloadSize,
                "Bloom mask size does not match the version-1 slot count"
            };
        }

        if (bloom_bits == 0 || bloom_bits != mask.size()) {
            return Status{
                StatusCode::InvalidPayloadSize,
                "Bloom slot count is zero or differs from the mask size"
            };
        }

        if (hash_count != BLOOM_HASH_COUNT || hash_count == 0) {
            return Status{
                StatusCode::NotSupported,
                "Bloom hash count is not supported by this format version"
            };
        }

        Status status = validate_mask_values(mask);
        if (!status.is_ok()) {
            return status;
        }

        const bool any_slot_set = std::any_of(
            mask.begin(),
            mask.end(),
            [](std::uint8_t value) { return value != 0; }
        );

        if ((key_count == 0 && any_slot_set) ||
            (key_count != 0 && !any_slot_set)) {
            return Status{
                StatusCode::InvalidState,
                "Bloom key count and mask contents are inconsistent"
            };
        }

        return Status::ok();
    }

    BloomSection::BloomSection() noexcept
    {
        payload.mask.fill(0u);
        payload.bloom_bits = payload.mask.size();
        payload.hash_count = BLOOM_HASH_COUNT;
        payload.key_count = 0;

        header.type = BlockType::Bloom;
        header.payload_size = static_cast<std::uint32_t>(Payload::disk_size());
        calculate_crc32(header.crc32);
    }

    Status BloomSection::validate() const
    {
        Status status = header.validate();
        if (!status.is_ok()) {
            return status;
        }

        status = payload.validate();
        if (!status.is_ok()) {
            return status;
        }

        std::uint32_t expected_crc = 0;
        calculate_crc32(expected_crc);
        if (header.crc32 != expected_crc) {
            return Status{
                StatusCode::ChecksumMismatch,
                "Bloom payload CRC does not match the cached header CRC"
            };
        }

        return Status::ok();
    }

    void BloomSection::Payload::calculate_crc32(
        std::uint32_t& crc_buffer
    ) const noexcept
    {
        crc_buffer = static_cast<std::uint32_t>(::crc32(0L, Z_NULL, 0));
        crc32_add_pod<std::uint64_t>(crc_buffer, bloom_bits);
        crc32_add_pod<std::uint32_t>(crc_buffer, hash_count);
        crc32_add_pod<std::uint32_t>(crc_buffer, key_count);
        compute_crc32(crc_buffer, mask.data(), mask.size());
    }

    void BloomSection::calculate_crc32(
        std::uint32_t& crc_buffer
    ) const noexcept
    {
        payload.calculate_crc32(crc_buffer);
    }

    Status BloomSection::recompute_crc32()
    {
        Status status = payload.validate();
        if (!status.is_ok()) {
            return status;
        }

        header.type = BlockType::Bloom;
        header.payload_size = static_cast<std::uint32_t>(Payload::disk_size());
        calculate_crc32(header.crc32);
        return Status::ok();
    }

    Status BloomSection::add_key(
        const void* key_ptr,
        std::uint32_t key_size
    )
    {
        return add_key_to_payload(payload, key_ptr, key_size);
    }

    Status BloomSection::rebuild(const DataSection& data_section)
    {
        Payload staged;
        staged.mask.fill(0u);
        staged.bloom_bits = staged.mask.size();
        staged.hash_count = BLOOM_HASH_COUNT;
        staged.key_count = 0;

        for (const auto& block : data_section.data_blocks) {
            for (const auto& record : block.payloads) {
                Status status = add_key_to_payload(
                    staged,
                    record.key_ptr,
                    record.key_size
                );
                if (!status.is_ok()) {
                    return status;
                }
            }
        }

        Header staged_header;
        staged_header.type = BlockType::Bloom;
        staged_header.payload_size =
            static_cast<std::uint32_t>(Payload::disk_size());
        staged.calculate_crc32(staged_header.crc32);

        payload = std::move(staged);
        header = staged_header;
        return Status::ok();
    }

    bool BloomSection::may_contain(
        const void* key_ptr,
        std::uint32_t key_size
    ) const noexcept
    {
        if (key_size > 0 && key_ptr == nullptr) {
            return true;
        }

        if (!payload.validate().is_ok()) {
            return true;
        }

        for (std::uint32_t i = 0; i < payload.hash_count; ++i) {
            const std::uint64_t hash = bloom_hash_i(key_ptr, key_size, i);
            const std::uint64_t slot = hash % payload.bloom_bits;
            if (payload.mask[static_cast<std::size_t>(slot)] == 0) {
                return false;
            }
        }

        return true;
    }

    Status BloomSection::Header::write(
        WritableFile& file,
        std::uint64_t& offset
    ) const
    {
        Status status = validate();
        if (!status.is_ok()) {
            return status;
        }

        status = validate_cursor(file, offset);
        if (!status.is_ok()) {
            return status;
        }

        status = fits_in_block(offset, disk_size(), BLOCK_SIZE);
        if (!status.is_ok()) {
            return status;
        }

        status = kvdb::blockio::write_u8_t(
            file,
            static_cast<std::uint8_t>(type),
            offset,
            BLOCK_SIZE
        );
        if (!status.is_ok()) {
            return status;
        }

        status = kvdb::blockio::write_u32_t_le(
            file,
            payload_size,
            offset,
            BLOCK_SIZE
        );
        if (!status.is_ok()) {
            return status;
        }

        return kvdb::blockio::write_u32_t_le(
            file,
            crc32,
            offset,
            BLOCK_SIZE
        );
    }

    Status BloomSection::Payload::write(
        WritableFile& file,
        std::uint64_t& offset
    ) const
    {
        Status status = validate();
        if (!status.is_ok()) {
            return status;
        }

        status = validate_cursor(file, offset);
        if (!status.is_ok()) {
            return status;
        }

        status = fits_in_block(offset, disk_size(), BLOCK_SIZE);
        if (!status.is_ok()) {
            return status;
        }

        status = kvdb::blockio::write_u64_t_le(
            file,
            bloom_bits,
            offset,
            BLOCK_SIZE
        );
        if (!status.is_ok()) {
            return status;
        }

        status = kvdb::blockio::write_u32_t_le(
            file,
            hash_count,
            offset,
            BLOCK_SIZE
        );
        if (!status.is_ok()) {
            return status;
        }

        status = kvdb::blockio::write_u32_t_le(
            file,
            key_count,
            offset,
            BLOCK_SIZE
        );
        if (!status.is_ok()) {
            return status;
        }

        return kvdb::blockio::write_bytes(
            file,
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(mask.data()),
                mask.size()
            ),
            offset,
            BLOCK_SIZE
        );
    }

    Status BloomSection::write(
        WritableFile& file,
        std::uint64_t& offset,
        std::uint64_t& bloom_offset
    )
    {
        Status status = validate_cursor(file, offset);
        if (!status.is_ok()) {
            return status;
        }

        status = payload.validate();
        if (!status.is_ok()) {
            return status;
        }

        // Rebuild derived header fields before any alignment padding or section
        // bytes are emitted.
        status = recompute_crc32();
        if (!status.is_ok()) {
            return status;
        }

        status = align_to_block_boundary(file, offset, BLOCK_SIZE);
        if (!status.is_ok()) {
            return status;
        }

        const std::uint64_t staged_bloom_offset = offset;
        status = fits_in_block(offset, disk_size(), BLOCK_SIZE);
        if (!status.is_ok()) {
            return status;
        }

        status = header.write(file, offset);
        if (!status.is_ok()) {
            return status;
        }

        status = payload.write(file, offset);
        if (!status.is_ok()) {
            return status;
        }

        bloom_offset = staged_bloom_offset;
        return Status::ok();
    }

    Result<BloomSection::Header> BloomSection::Header::load(
        ReadableFile& file,
        std::uint64_t& offset
    )
    {
        const std::uint64_t initial_offset = offset;
        Header result;
        std::uint8_t encoded_type = 0;

        Status status = fits_in_block(offset, disk_size(), BLOCK_SIZE);
        if (!status.is_ok()) {
            return Result<Header>::fail(std::move(status));
        }

        status = kvdb::blockio::read_u8_t(
            file,
            encoded_type,
            offset,
            BLOCK_SIZE
        );
        if (!status.is_ok()) {
            offset = initial_offset;
            return Result<Header>::fail(std::move(status));
        }

        result.type = static_cast<BlockType>(encoded_type);
        if (result.type != BlockType::Bloom) {
            offset = initial_offset;
            return Result<Header>::fail(Status{
                StatusCode::InvalidBlockType,
                "expected a Bloom block header"
                });
        }

        status = kvdb::blockio::read_u32_t_le(
            file,
            result.payload_size,
            offset,
            BLOCK_SIZE
        );
        if (!status.is_ok()) {
            offset = initial_offset;
            return Result<Header>::fail(std::move(status));
        }

        if (result.payload_size != Payload::disk_size()) {
            offset = initial_offset;
            return Result<Header>::fail(Status{
                StatusCode::InvalidPayloadSize,
                "Bloom payload size is not valid for the version-1 format"
                });
        }

        status = kvdb::blockio::read_u32_t_le(
            file,
            result.crc32,
            offset,
            BLOCK_SIZE
        );
        if (!status.is_ok()) {
            offset = initial_offset;
            return Result<Header>::fail(std::move(status));
        }

        return Result<Header>::ok(std::move(result));
    }

    Result<BloomSection::Payload> BloomSection::Payload::load(
        ReadableFile& file,
        std::uint64_t& offset
    )
    {
        const std::uint64_t initial_offset = offset;
        Payload result;

        Status status = fits_in_block(offset, disk_size(), BLOCK_SIZE);
        if (!status.is_ok()) {
            return Result<Payload>::fail(std::move(status));
        }

        status = kvdb::blockio::read_u64_t_le(
            file,
            result.bloom_bits,
            offset,
            BLOCK_SIZE
        );
        if (!status.is_ok()) {
            offset = initial_offset;
            return Result<Payload>::fail(std::move(status));
        }

        status = kvdb::blockio::read_u32_t_le(
            file,
            result.hash_count,
            offset,
            BLOCK_SIZE
        );
        if (!status.is_ok()) {
            offset = initial_offset;
            return Result<Payload>::fail(std::move(status));
        }

        status = kvdb::blockio::read_u32_t_le(
            file,
            result.key_count,
            offset,
            BLOCK_SIZE
        );
        if (!status.is_ok()) {
            offset = initial_offset;
            return Result<Payload>::fail(std::move(status));
        }

        status = kvdb::blockio::read_bytes(
            file,
            reinterpret_cast<std::byte*>(result.mask.data()),
            static_cast<std::uint32_t>(result.mask.size()),
            offset,
            BLOCK_SIZE
        );
        if (!status.is_ok()) {
            offset = initial_offset;
            return Result<Payload>::fail(std::move(status));
        }

        return Result<Payload>::ok(std::move(result));
    }

    Result<BloomSection> BloomSection::load(
        ReadableFile& file,
        std::uint64_t& offset,
        std::uint64_t bloom_offset
    )
    {
        const std::uint64_t initial_offset = offset;

        if (bloom_offset % BLOCK_SIZE != 0) {
            return Result<BloomSection>::fail(Status{
                StatusCode::InvalidBlockAlignment,
                "Bloom section offset is not block-aligned"
                });
        }

        std::uint64_t file_size = 0;
        Status status = file.get_file_size(file_size);
        if (!status.is_ok()) {
            return Result<BloomSection>::fail(std::move(status));
        }

        status = can_read_range(bloom_offset, disk_size(), file_size);
        if (!status.is_ok()) {
            return Result<BloomSection>::fail(std::move(status));
        }

        offset = bloom_offset;

        Result<Header> header_result = Header::load(file, offset);
        if (!header_result.is_ok()) {
            offset = initial_offset;
            return Result<BloomSection>::fail(std::move(header_result.status));
        }

        Result<Payload> payload_result = Payload::load(file, offset);
        if (!payload_result.is_ok()) {
            offset = initial_offset;
            return Result<BloomSection>::fail(std::move(payload_result.status));
        }

        BloomSection result;
        result.header = std::move(header_result.value);
        result.payload = std::move(payload_result.value);

        std::uint32_t expected_crc = 0;
        result.calculate_crc32(expected_crc);
        if (expected_crc != result.header.crc32) {
            offset = initial_offset;
            return Result<BloomSection>::fail(Status{
                StatusCode::ChecksumMismatch,
                "Bloom payload CRC mismatch"
                });
        }

        status = result.payload.validate();
        if (!status.is_ok()) {
            offset = initial_offset;
            return Result<BloomSection>::fail(std::move(status));
        }

        return Result<BloomSection>::ok(std::move(result));
    }
}