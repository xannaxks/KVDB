#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "arena.h"
#include "crc32_helpers.h"
#include "endian_io.h"
#include "file.h"
#include "file_helpers.h"
#include "sstable_entities.h"
#include "status.h"

namespace SSTableEntities
{
    struct IndexSection
    {
        struct Header
        {
            BlockType type = BlockType::Index;
            std::uint32_t payload_size = 0;
            std::uint32_t crc32 = 0;

            Header() noexcept = default;
            Header(const Header&) noexcept = default;
            Header(Header&&) noexcept = default;
            Header& operator=(const Header&) noexcept = default;
            Header& operator=(Header&&) noexcept = default;

            [[nodiscard]] static constexpr std::size_t disk_size() noexcept
            {
                // Encoded widths, not sizeof(BlockType).
                return sizeof(std::uint8_t) +
                    sizeof(std::uint32_t) +
                    sizeof(std::uint32_t);
            }

            [[nodiscard]] Status validate() const;

            // Direct header I/O never aligns. The containing IndexSection owns
            // section alignment and must preflight the whole section first.
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
            std::uint64_t data_block_offset = 0;
            std::uint32_t first_key_size = 0;
            std::uint32_t last_key_size = 0;

            // Construction borrows immutable DataSection key bytes. Loading
            // points these fields at Arena-owned copies.
            const void* first_key_ptr = nullptr;
            const void* last_key_ptr = nullptr;

            Payload() noexcept = default;
            Payload(const Payload&) noexcept = default;
            Payload(Payload&&) noexcept = default;
            Payload& operator=(const Payload&) noexcept = default;
            Payload& operator=(Payload&&) noexcept = default;

            [[nodiscard]] static constexpr std::size_t fixed_disk_size() noexcept
            {
                return sizeof(std::uint64_t) +
                    sizeof(std::uint32_t) +
                    sizeof(std::uint32_t);
            }

            [[nodiscard]] std::size_t disk_size() const noexcept;
            [[nodiscard]] Status validate() const;

            [[nodiscard]] std::span<const std::byte> first_key() const noexcept;
            [[nodiscard]] std::span<const std::byte> last_key() const noexcept;

            [[nodiscard]] Status write(
                WritableFile& file,
                std::uint64_t& offset
            ) const;

            void calculate_crc32(
                std::uint32_t& crc_buffer
            ) const noexcept;

            void append_crc32(
                std::uint32_t& crc_buffer
            ) const noexcept;
        };

        struct CandidateRange
        {
            std::size_t first = 0;
            std::size_t last_exclusive = 0;

            [[nodiscard]] bool empty() const noexcept
            {
                return first == last_exclusive;
            }

            [[nodiscard]] std::size_t size() const noexcept
            {
                return last_exclusive - first;
            }
        };

        IndexSection() noexcept;

        IndexSection(const IndexSection&) = default;
        IndexSection(IndexSection&&) noexcept = default;
        IndexSection& operator=(const IndexSection&) = default;
        IndexSection& operator=(IndexSection&&) noexcept = default;

        Header header;
        std::vector<Payload> payloads;

        [[nodiscard]] static constexpr std::size_t fixed_disk_size() noexcept
        {
            return Header::disk_size() + Payload::fixed_disk_size();
        }

        // Logical encoded bytes. Version 1 requires this entire value to fit in
        // one physical block.
        [[nodiscard]] std::size_t disk_size() const noexcept;

        // Validates the current public header and payloads, including exact
        // derived payload size and CRC.
        [[nodiscard]] Status validate() const;

        // Recomputes type, payload_size, and CRC from the payload vector.
        [[nodiscard]] Status rebuild_header();

        // Verifies that one index entry exists for every contiguous physical
        // data block starting at first_data_block_offset.
        [[nodiscard]] Status validate_data_layout(
            std::uint64_t first_data_block_offset,
            std::uint64_t data_block_count
        ) const;

        // Transactional with respect to this object. The pointed-to key bytes are
        // borrowed and must outlive index serialization.
        [[nodiscard]] Status add_index(
            std::uint64_t data_block_offset,
            std::uint32_t first_key_size,
            std::uint32_t last_key_size,
            const void* first_key_ptr,
            const void* last_key_ptr
        );

        // Validates and rebuilds the header before its first output byte.
        // header and index_offset are committed only after complete success.
        [[nodiscard]] Status write(
            WritableFile& file,
            std::uint64_t& offset,
            std::uint64_t& index_offset
        );

        // Compatibility loader. A nonzero expected_index_size should normally be
        // footer.index_size. Zero means no independent size was supplied.
        [[nodiscard]] static Result<IndexSection> load(
            ReadableFile& file,
            std::uint64_t& offset,
            Arena& arena,
            std::uint64_t index_offset,
            std::uint32_t expected_index_size = 0
        );

        // Strict loader used by SSTable::load(). In addition to section size, it
        // verifies entry count and every data-block offset against the footer.
        [[nodiscard]] static Result<IndexSection> load(
            ReadableFile& file,
            std::uint64_t& offset,
            Arena& arena,
            std::uint64_t index_offset,
            std::uint32_t expected_index_size,
            std::uint64_t first_data_block_offset,
            std::uint64_t data_block_count
        );

        // Returns every block whose inclusive [first_key,last_key] range contains
        // key. Adjacent ranges may share one boundary key when versions of that
        // user key span physical blocks.
        [[nodiscard]] Result<CandidateRange> find_candidate_range(
            const void* key_ptr,
            std::uint32_t key_size
        ) const;

        // For an ordinary latest-value lookup, the earliest candidate contains
        // the newest versions because data records are globally ordered by key
        // ascending and sequence number descending.
        [[nodiscard]] Result<std::size_t> find_first_candidate(
            const void* key_ptr,
            std::uint32_t key_size
        ) const;

        void calculate_crc32(
            std::uint32_t& crc_buffer
        ) const noexcept;
    };
}