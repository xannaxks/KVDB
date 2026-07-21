#pragma once

#include <cstddef>
#include <cstdint>

#include "arena.h"
#include "crc32_helpers.h"
#include "file.h"
#include "sstable_entities.h"
#include "sstable_entities/data_section.h"
#include "sstable_entities/index_section.h"
#include "status.h"

namespace SSTableEntities
{
    struct MetaSection
    {
        struct Header
        {
            Header() noexcept = default;

            Header(const Header&) noexcept = default;
            Header(Header&&) noexcept = default;
            Header& operator=(const Header&) noexcept = default;
            Header& operator=(Header&&) noexcept = default;

            BlockType type = BlockType::Meta;
            std::uint32_t payload_size = 0;
            std::uint32_t crc32 = 0;

            [[nodiscard]] static constexpr std::size_t disk_size() noexcept
            {
                // Stable on-disk widths: u8 type, u32 payload size, u32 CRC.
                return sizeof(std::uint8_t) +
                    sizeof(std::uint32_t) +
                    sizeof(std::uint32_t);
            }

            [[nodiscard]] static constexpr std::size_t fixed_disk_size() noexcept
            {
                return disk_size();
            }

            [[nodiscard]] Status validate() const;

            // Does not align. MetaSection::write() owns section alignment.
            [[nodiscard]] Status write(
                WritableFile& file,
                std::uint64_t& offset
            ) const;

            // Does not align. The caller must position offset at the header.
            [[nodiscard]] static Result<Header> load(
                ReadableFile& file,
                std::uint64_t& offset
            );
        };

        // min_key_ptr and max_key_ptr are non-owning after rebuild(). They point
        // into DataSection record storage and must remain alive and unchanged
        // through metadata serialization. After load(), they point into Arena.
        struct Payload
        {
            Payload() noexcept = default;

            Payload(const Payload&) noexcept = default;
            Payload(Payload&&) noexcept = default;
            Payload& operator=(const Payload&) noexcept = default;
            Payload& operator=(Payload&&) noexcept = default;

            std::uint64_t record_count = 0;
            std::uint64_t tombstone_count = 0;
            std::uint64_t min_seq_num = 0;
            std::uint64_t max_seq_num = 0;

            std::uint32_t min_key_size = 0;
            std::uint32_t max_key_size = 0;

            std::uint64_t data_block_count = 0;

            // Sum of encoded bytes actually used by data blocks. Alignment gaps
            // between data blocks are intentionally excluded.
            std::uint64_t data_bytes = 0;

            void* max_key_ptr = nullptr;
            void* min_key_ptr = nullptr;

            [[nodiscard]] static constexpr std::size_t fixed_disk_size() noexcept
            {
                return 6u * sizeof(std::uint64_t) +
                    2u * sizeof(std::uint32_t);
            }

            [[nodiscard]] std::size_t disk_size() const noexcept;
            [[nodiscard]] Status validate() const;

            // Does not align or split. The complete payload must fit in the
            // current physical block.
            [[nodiscard]] Status write(
                WritableFile& file,
                std::uint64_t& offset
            ) const;

            [[nodiscard]] static Result<Payload> load(
                ReadableFile& file,
                std::uint64_t& offset,
                Arena& arena,
                const Header& header
            );

            void calculate_crc32(
                std::uint32_t& crc_buffer
            ) const noexcept;
        };

        MetaSection() noexcept;

        MetaSection(const MetaSection&) noexcept = default;
        MetaSection(MetaSection&&) noexcept = default;
        MetaSection& operator=(const MetaSection&) noexcept = default;
        MetaSection& operator=(MetaSection&&) noexcept = default;

        Header header;
        Payload payload;

        [[nodiscard]] static constexpr std::size_t fixed_disk_size() noexcept
        {
            return Header::disk_size() + Payload::fixed_disk_size();
        }

        [[nodiscard]] std::size_t disk_size() const noexcept;

        // Rebuilds transactionally. On failure, this MetaSection is unchanged.
        [[nodiscard]] Status rebuild(
            const DataSection& data_section,
            const IndexSection& index_section
        );

        [[nodiscard]] Status validate(
            const IndexSection& index_section
        ) const;

        // Physical output and offset cannot be rolled back after an I/O failure.
        // meta_offset and this->header are committed only after full success.
        [[nodiscard]] Status write(
            WritableFile& file,
            std::uint64_t& offset,
            std::uint64_t& meta_offset
        );

        // On failure, restores the caller's offset and rolls the Arena back to
        // its entry checkpoint.
        [[nodiscard]] static Result<MetaSection> load(
            ReadableFile& file,
            std::uint64_t& offset,
            const IndexSection& index_section,
            Arena& arena,
            std::uint64_t meta_offset = 0
        );

        void calculate_crc32(
            std::uint32_t& crc_buffer
        ) const noexcept;
    };
}