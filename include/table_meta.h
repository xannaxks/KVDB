#pragma once

#include <cstdint>
#include <filesystem>
#include "arena.h"
#include "sstable.h"
#include "status.h"

constexpr int MANIFEST_BLOCK_SIZE = 4096;

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

    void calculate_crc(std::uint32_t& crc_buffer, bool init = false);

    Status write(WritableFile& file, std::uint64_t& offset);
    static Result<TableMeta> load(ReadableFile& file, std::uint64_t& offset, Arena& arena);
};

Result<TableMeta> make_table_meta(
	const SSTable& sstable,
	std::uint32_t level,
    Arena& arena
);