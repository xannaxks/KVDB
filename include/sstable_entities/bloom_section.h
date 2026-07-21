#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "sstable_entities.h"
#include "status.h"
#include "file.h"

namespace SSTableEntities
{
    struct BloomSection
    {
        struct Header
        {
            Header() = default;
            Header(const Header&) = default;
            Header(Header&&) noexcept = default;
            Header& operator=(const Header&) = default;
            Header& operator=(Header&&) noexcept = default;

            BlockType type = BlockType::Bloom;
            std::uint32_t payload_size = 0;
            std::uint32_t crc32 = 0;

            [[nodiscard]] static constexpr std::size_t disk_size() noexcept
            {
                return sizeof(std::uint8_t) +
                    sizeof(std::uint32_t) +
                    sizeof(std::uint32_t);
            }

            [[nodiscard]] Status validate() const;
            [[nodiscard]] Status write(
                WritableFile& file,
                std::uint64_t& offset
            ) const;

            [[nodiscard]] static Result<Header> load(
                ReadableFile& file,
                std::uint64_t& offset
            );
        };

        struct Payload
        {
            Payload() = default;
            Payload(const Payload&) = default;
            Payload(Payload&&) noexcept = default;
            Payload& operator=(const Payload&) = default;
            Payload& operator=(Payload&&) noexcept = default;

            // Version-1 format semantics: bloom_bits is the number of
            // byte-addressed Boolean slots, not the number of packed bits.
            std::uint64_t bloom_bits = 0;
            std::uint32_t hash_count = 0;
            std::uint32_t key_count = 0;
            std::array<std::uint8_t, BLOOM_MASK_BIT_SIZE> mask{};

            [[nodiscard]] static constexpr std::size_t fixed_part_disk_size() noexcept
            {
                return sizeof(std::uint64_t) +
                    sizeof(std::uint32_t) +
                    sizeof(std::uint32_t);
            }

            [[nodiscard]] static constexpr std::size_t disk_size() noexcept
            {
                return fixed_part_disk_size() + BLOOM_MASK_BIT_SIZE;
            }

            [[nodiscard]] Status validate() const;
            [[nodiscard]] Status write(
                WritableFile& file,
                std::uint64_t& offset
            ) const;

            [[nodiscard]] static Result<Payload> load(
                ReadableFile& file,
                std::uint64_t& offset
            );

            void calculate_crc32(std::uint32_t& crc_buffer) const noexcept;
        };

        BloomSection() noexcept;
        BloomSection(const BloomSection&) noexcept = default;
        BloomSection(BloomSection&&) noexcept = default;
        BloomSection& operator=(const BloomSection&) = default;
        BloomSection& operator=(BloomSection&&) noexcept = default;

        Header header;
        Payload payload;

        [[nodiscard]] static constexpr std::size_t disk_size() noexcept
        {
            return Header::disk_size() + Payload::disk_size();
        }

        [[nodiscard]] Status validate() const;

        // On a physical write failure, already-written bytes and offset cannot
        // be rolled back here. bloom_offset is committed only after success.
        [[nodiscard]] Status write(
            WritableFile& file,
            std::uint64_t& offset,
            std::uint64_t& bloom_offset
        );

        [[nodiscard]] static Result<BloomSection> load(
            ReadableFile& file,
            std::uint64_t& offset,
            std::uint64_t bloom_offset
        );

        [[nodiscard]] Status add_key(
            const void* key_ptr,
            std::uint32_t key_size
        );

        // Rebuild is transactional with respect to the in-memory BloomSection.
        [[nodiscard]] Status rebuild(const DataSection& data_section);
        [[nodiscard]] Status recompute_crc32();

        // A malformed filter must fail open: true means the caller must still
        // check the SSTable. Returning false from invalid state could create a
        // Bloom-filter false negative and hide a real key.
        [[nodiscard]] bool may_contain(
            const void* key_ptr,
            std::uint32_t key_size
        ) const noexcept;

        void calculate_crc32(std::uint32_t& crc_buffer) const noexcept;
    };
}