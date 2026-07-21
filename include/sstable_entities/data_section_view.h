#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "arena.h"
#include "data_section.h"
#include "file.h"
#include "record.h"
#include "sstable_entities.h"
#include "status.h"

namespace SSTableEntities
{
    struct DataSectionView
    {
        struct Header
        {
            DataSection::Header header{};

            std::uint64_t header_offset = 0;
            std::uint64_t payload_offset = 0;
            std::uint64_t payload_end_offset = 0;
            std::uint64_t next_block_offset = 0;

            [[nodiscard]] std::uint64_t used_size() const noexcept;

            // Reads only the nine-byte data-block header. The caller-provided
            // section_end_offset prevents a corrupt block count from walking into
            // the following SSTable section.
            [[nodiscard]] static Result<Header> load(
                ReadableFile& file,
                std::uint64_t& offset,
                std::uint64_t section_end_offset
            );
        };

        struct RecordMeta
        {
            std::uint32_t key_size = 0;
            std::uint32_t value_size = 0;
            ::Type type = ::Type::Put;
            std::uint64_t seq_num = 0;

            std::uint64_t record_offset = 0;
            std::uint64_t key_offset = 0;
            std::uint64_t value_offset = 0;

            [[nodiscard]] std::uint64_t disk_size() const noexcept;
        };

        struct DataBlock
        {
            enum class ValidationState : std::uint8_t
            {
                Unvalidated,
                Valid,
                Corrupt
            };

            Header header_view{};

            DataBlock() = default;
            DataBlock(const DataBlock&) = default;
            DataBlock(DataBlock&&) noexcept = default;
            DataBlock& operator=(const DataBlock&) = default;
            DataBlock& operator=(DataBlock&&) noexcept = default;

            [[nodiscard]] std::uint64_t used_size() const noexcept;
            [[nodiscard]] ValidationState validation_state() const noexcept;
            [[nodiscard]] std::size_t record_count() const noexcept;

            // Header-only discovery. No payload bytes are read here.
            [[nodiscard]] static Result<DataBlock> load(
                ReadableFile& file,
                std::uint64_t& offset,
                std::uint64_t section_end_offset
            );

            // On first use this reads at most one physical block payload, checks
            // its CRC over the exact serialized bytes, validates every record
            // boundary and field, and caches only lightweight RecordMeta entries.
            // A deterministic corruption result is cached. Transient I/O failures
            // are not cached and may be retried.
            [[nodiscard]] Status validate(ReadableFile& file) const;

            // Requires an immutable SSTable file. validate() is called first;
            // afterwards only this record's key and value bytes are copied into
            // the supplied Arena. Arena state is rolled back on failure.
            [[nodiscard]] Result<InternalRecord> read_record(
                ReadableFile& file,
                std::size_t record_index,
                Arena& arena
            ) const;
            [[nodiscard]] Result<std::optional<std::size_t>> find_first_record(
                ReadableFile& file,
                const ArenaEntry& key
            ) const;

        private:
            mutable ValidationState validation_state_ = ValidationState::Unvalidated;
            mutable std::optional<Status> cached_corruption_{};
            mutable std::vector<RecordMeta> records_{};
        };

        DataSectionView() = default;
        DataSectionView(const DataSectionView&) = default;
        DataSectionView(DataSectionView&&) noexcept = default;
        DataSectionView& operator=(const DataSectionView&) = default;
        DataSectionView& operator=(DataSectionView&&) noexcept = default;

        std::vector<DataBlock> data_blocks{};

        std::uint64_t first_data_block_offset = 0;
        std::uint64_t section_end_offset = 0;

        // first_data_block_offset is always an absolute offset; zero is not a
        // sentinel. For non-empty data sections it must be block aligned.
        // expected_section_end_offset should normally be footer.index_offset.
        // Pass zero only when the caller has no independently trusted section end.
        [[nodiscard]] static Result<DataSectionView> load(
            ReadableFile& file,
            std::uint64_t& offset,
            std::uint64_t first_data_block_offset,
            std::uint64_t data_block_count,
            std::uint64_t expected_section_end_offset = 0
        );

        [[nodiscard]] Result<std::uint64_t> logical_size() const;

        // Span from the first block header through the last used payload byte.
        // Includes inter-block padding, but excludes trailing padding after the
        // last used byte.
        [[nodiscard]] Result<std::uint64_t> physical_span() const;

        // Space reserved up to the next aligned section: block_count * BLOCK_SIZE.
        [[nodiscard]] Result<std::uint64_t> reserved_span() const;

        [[nodiscard]] Status validate_block(
            ReadableFile& file,
            std::size_t block_index
        ) const;

        [[nodiscard]] Result<InternalRecord> read_record(
            ReadableFile& file,
            std::size_t block_index,
            std::size_t record_index,
            Arena& arena
        ) const;
        [[nodiscard]] Result<std::optional<std::size_t>> find_first_record(
            ReadableFile& file,
            std::size_t block_index,
            const ArenaEntry& key
        ) const;
    };
}