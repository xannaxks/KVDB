#include <cstdint>
#include <filesystem>
#include "arena.h"
#include "sstable.h"
#include "status.h"

struct TableMeta
{
    std::uint64_t table_id = 0;
    std::uint32_t level = 0;

    std::filesystem::path path;

    ArenaEntry smallest_key{};
    ArenaEntry largest_key{};

    std::uint64_t min_seq = 0;
    std::uint64_t max_seq = 0;

    std::uint64_t file_size = 0;
    std::uint64_t record_count = 0;
    std::uint64_t tombstone_count = 0;
    std::uint64_t data_block_count = 0;
    std::uint64_t data_bytes = 0;
};
Result<TableMeta> make_table_meta(
	const SSTable& sstable,
	std::uint32_t level,
    Arena& arena
);