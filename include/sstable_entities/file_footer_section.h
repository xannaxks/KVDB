#pragma once

#include <cstddef>
#include <cstdint>

#include "file.h"
#include "sstable_entities.h"
#include "status.h"

namespace SSTableEntities
{
    struct FileFooterSection
    {
        FileFooterSection() noexcept;

        FileFooterSection(const FileFooterSection&) noexcept = default;
        FileFooterSection(FileFooterSection&&) noexcept = default;
        FileFooterSection& operator=(const FileFooterSection&) noexcept = default;
        FileFooterSection& operator=(FileFooterSection&&) noexcept = default;

        std::uint32_t magic = FILE_FOOTER_MAGIC;
        std::uint32_t version = LATEST_SSTABLE_VERSION;
        std::uint32_t reserved = 0;

        std::uint64_t data_offset = 0;
        std::uint64_t data_block_count = 0;

        std::uint64_t index_offset = 0;
        std::uint32_t index_size = 0;

        std::uint64_t bloom_offset = 0;
        std::uint32_t bloom_size = 0;

        std::uint64_t meta_offset = 0;
        std::uint32_t meta_size = 0;

        std::uint64_t file_size = 0;
        std::uint32_t footer_crc32 = 0;

        [[nodiscard]] static constexpr std::size_t disk_size() noexcept
        {
            // Stable version-1 encoded widths. Do not use sizeof(*this): the
            // in-memory structure may contain padding.
            return 3u * sizeof(std::uint32_t) +
                2u * sizeof(std::uint64_t) +
                sizeof(std::uint64_t) + sizeof(std::uint32_t) +
                sizeof(std::uint64_t) + sizeof(std::uint32_t) +
                sizeof(std::uint64_t) + sizeof(std::uint32_t) +
                sizeof(std::uint64_t) +
                sizeof(std::uint32_t);
        }

        // Validates a complete version-1 footer against its physical location
        // and the actual file size. This includes CRC validation.
        [[nodiscard]] Status validate(
            std::uint64_t footer_offset,
            std::uint64_t actual_file_size
        ) const;

        // Rebuilds the derived footer fields transactionally. The writable file
        // must currently end exactly at footer_offset.
        [[nodiscard]] Status finalize(
            WritableFile& file,
            std::uint64_t footer_offset
        );

        // On failure, offset remains unchanged. A nonzero backwards offset is
        // measured from EOF. The default reads the fixed-size footer at EOF.
        [[nodiscard]] static Result<FileFooterSection> load(
            ReadableFile& file,
            std::uint64_t& offset,
            std::uint64_t file_footer_backwards_offset = disk_size()
        );

        [[nodiscard]] Status rebuild(
            const DataSection& data_section,
            std::uint64_t data_section_offset
        );

        [[nodiscard]] Status rebuild(
            IndexSection& index_section,
            std::uint64_t index_section_offset
        );

        [[nodiscard]] Status rebuild(
            BloomSection& bloom_section,
            std::uint64_t bloom_section_offset
        );

        [[nodiscard]] Status rebuild(
            MetaSection& meta_section,
            std::uint64_t meta_section_offset
        );

        // write() stages finalization and commits this object only after every
        // footer byte has been written. Physical bytes and offset cannot be
        // rolled back after an I/O failure.
        [[nodiscard]] Status write(
            WritableFile& file,
            std::uint64_t& offset
        );

        void calculate_crc32(
            std::uint32_t& crc_buffer
        ) const noexcept;
    };
}