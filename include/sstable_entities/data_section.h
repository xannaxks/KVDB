#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "crc32_helpers.h"
#include "file.h"
#include "record.h"
#include "sstable_entities.h"
#include "sstable_entities/index_section.h"
#include "status.h"

namespace SSTableEntities
{
    struct DataSection
    {
        struct Header
        {
            Header() noexcept;

            Header(const Header&) noexcept = default;
            Header(Header&&) noexcept = default;
            Header& operator=(const Header&) noexcept = default;
            Header& operator=(Header&&) noexcept = default;

            BlockType type = BlockType::Data;
            std::uint32_t payload_disk_size = 0;
            std::uint32_t crc32 = 0;

            [[nodiscard]] static constexpr std::size_t disk_size() noexcept
            {
                // On-disk widths, not C++ object sizes.
                return sizeof(std::uint8_t) +
                    sizeof(std::uint32_t) +
                    sizeof(std::uint32_t);
            }

            [[nodiscard]] Status write(
                WritableFile& file,
                std::uint64_t& offset
            ) const;
        };

        // Non-owning view of an InternalRecord. The pointed-to bytes must remain
        // alive and unchanged until both data and derived index serialization end.
        struct Payload
        {
            Payload() noexcept = default;
            explicit Payload(const InternalRecord& record) noexcept;

            Payload(const Payload&) noexcept = default;
            Payload(Payload&&) noexcept = default;
            Payload& operator=(const Payload&) noexcept = default;
            Payload& operator=(Payload&&) noexcept = default;

            std::uint32_t key_size = 0;
            std::uint32_t value_size = 0;
            ::Type type = ::Type::Put;
            std::uint32_t flags = 0;
            std::uint32_t reserved = 0;
            std::uint64_t seq_num = 0;
            void* key_ptr = nullptr;
            void* value_ptr = nullptr;

            [[nodiscard]] static constexpr std::size_t fixed_part_disk_size() noexcept
            {
                // key_size, value_size, type, flags, reserved, seq_num.
                return sizeof(std::uint32_t) +
                    sizeof(std::uint32_t) +
                    sizeof(std::uint8_t) +
                    sizeof(std::uint32_t) +
                    sizeof(std::uint32_t) +
                    sizeof(std::uint64_t);
            }

            [[nodiscard]] std::size_t disk_size() const noexcept;
            [[nodiscard]] Status validate() const;

            [[nodiscard]] Status write(
                WritableFile& file,
                std::uint64_t& offset
            ) const;

            void calculate_crc32(std::uint32_t& crc_buffer) const noexcept;
            void append_crc32(std::uint32_t& crc_buffer) const noexcept;
        };

        struct DataBlock
        {
            DataBlock() noexcept = default;

            DataBlock(const DataBlock&) = default;
            DataBlock(DataBlock&&) noexcept = default;
            DataBlock& operator=(const DataBlock&) = default;
            DataBlock& operator=(DataBlock&&) noexcept = default;

            Header header;
            std::vector<Payload> payloads;

            [[nodiscard]] std::size_t disk_size() const noexcept;
            [[nodiscard]] bool can_payload_fit(const Payload& payload) const noexcept;
            [[nodiscard]] Status validate() const;

            [[nodiscard]] Status add_payload(const Payload& payload);
            [[nodiscard]] Status rebuild_header();

            [[nodiscard]] Status write(
                WritableFile& file,
                std::uint64_t& offset,
                IndexSection& index_section
            );

            void calculate_crc32(std::uint32_t& crc_buffer) const noexcept;
        };

        DataSection() noexcept = default;

        DataSection(const DataSection&) = default;
        DataSection(DataSection&&) noexcept = default;
        DataSection& operator=(const DataSection&) = default;
        DataSection& operator=(DataSection&&) noexcept = default;

        std::vector<DataBlock> data_blocks;

        // Kept for compatibility. Empty blocks are rejected by validate()/write().
        // Prefer add_payload() for normal construction.
        void init_new_block();

        [[nodiscard]] Status add_payload(const InternalRecord& record);
        [[nodiscard]] Status validate() const;

        // Sum of used encoded bytes. This intentionally excludes inter-block
        // alignment padding. Use physical_span() for an offset/extent calculation.
        [[nodiscard]] std::size_t disk_size() const noexcept;

        // Bytes from the first block start through the last used byte, including
        // alignment gaps between blocks but excluding trailing padding after the
        // last block.
        [[nodiscard]] std::size_t physical_span() const noexcept;

        // Physical writes and offset cannot be rolled back after an I/O failure.
        // data_offset and index_section are committed only after full success.
        [[nodiscard]] Status write(
            WritableFile& file,
            std::uint64_t& offset,
            IndexSection& index_section,
            std::uint64_t& data_offset
        );
    };
}