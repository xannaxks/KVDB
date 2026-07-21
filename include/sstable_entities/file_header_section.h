#pragma once

#include <cstddef>
#include <cstdint>

#include "status.h"
#include "file_helpers.h"
#include "crc32_helpers.h"
#include "file.h"
#include "endian_io.h"
#include "sstable_entities.h"

namespace SSTableEntities
{
    struct FileHeaderSection
    {
        FileHeaderSection() noexcept;
        explicit FileHeaderSection(std::uint32_t table_id) noexcept;

        FileHeaderSection(const FileHeaderSection&) noexcept = default;
        FileHeaderSection(FileHeaderSection&&) noexcept = default;
        FileHeaderSection& operator=(const FileHeaderSection&) noexcept = default;
        FileHeaderSection& operator=(FileHeaderSection&&) noexcept = default;

        std::uint32_t magic = FILE_HEADER_MAGIC;
        std::uint32_t version = LATEST_SSTABLE_VERSION;
        std::uint32_t flags = 0;
        std::uint32_t block_size = BLOCK_SIZE;
        std::uint32_t table_id = 0;
        std::uint32_t crc32 = 0;

        [[nodiscard]] static constexpr std::size_t disk_size() noexcept
        {
            return 6u * sizeof(std::uint32_t);
        }

        Status write(WritableFile& file, std::uint64_t& offset);
        [[nodiscard]] static Result<FileHeaderSection> load(
            ReadableFile& file,
            std::uint64_t& offset
        );

        Status calculate_crc32(std::uint32_t& crc_buffer) const noexcept;
    };
}