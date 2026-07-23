#pragma once

#include <cstdint>
#include <filesystem>

#include "arena.h"
#include "sstable.h"
#include "status.h"

inline constexpr std::uint32_t MANIFEST_BLOCK_SIZE = 4096;
inline constexpr std::uint64_t MAX_TABLE_META_VARIABLE_BYTES =
static_cast<std::uint64_t>(MANIFEST_BLOCK_SIZE) * 16ull;

struct TableMeta
{
    std::uint64_t table_id = 0;
    std::uint32_t level = 0;

    std::filesystem::path path;

    std::uint64_t min_seq = 0;
    std::uint64_t max_seq = 0;

    std::uint64_t file_size = 0;
    std::uint64_t record_count = 0;
    std::uint64_t tombstone_count = 0;
    std::uint64_t data_block_count = 0;
    std::uint64_t data_bytes = 0;

    ArenaEntry smallest_key{};
    ArenaEntry largest_key{};

    // Checks invariants that are intrinsic to TableMeta and its on-disk format.
    // Level-range/table-id policy belongs to higher layers because TableMeta does
    // not know DBOptions/max_levels or whether table id 0 is reserved.
    [[nodiscard]] Status validate() const;

    // Result<> avoids silent uint32_t truncation for malformed/in-memory values.
    [[nodiscard]] Result<std::uint32_t> disk_size() const;

    // CRC is only produced for a valid/serializable TableMeta.
    [[nodiscard]] Status calculate_crc(std::uint32_t& crc_buffer, bool init = false) const;

    // All validation happens before the first physical write. After I/O begins,
    // an underlying write failure may still leave a torn tail in the file; the
    // manifest record/framing layer should detect/ignore that during recovery.
    [[nodiscard]] Status write(WritableFile& file, std::uint64_t& offset) const;

    // Transactional with respect to the caller-visible offset and Arena:
    // on failure, offset is unchanged and Arena allocations made by this load
    // are rolled back.
    [[nodiscard]] static Result<TableMeta> load(
        ReadableFile& file,
        std::uint64_t& offset,
        Arena& arena
    );
};

[[nodiscard]] Result<TableMeta> make_table_meta(
    const SSTable& sstable,
    std::uint32_t level,
    Arena& arena
);  