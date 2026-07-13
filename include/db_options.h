#pragma once
#include <filesystem>
#include <cstdint>

struct DBOptions
{
	std::filesystem::path db_path;
	std::size_t mem_table_size_limit = 64 * 1024 * 1024;
	std::size_t BLOCK_SIZE = 4096;

	bool create_if_missing = true;
	bool error_if_exists = false;

	bool enable_background_compaction = true;

	struct CompactionSchedulerOptions
	{
		std::size_t max_compaction_threads = 4;
		std::size_t compaction_trigger_threshold = 4;
		std::size_t compaction_max_level = 7;
	} compaction_options;

	struct WALOptions
	{
		std::size_t wal_file_size_limit = 64 * 1024 * 1024;
		std::filesystem::path path;
	} wal_options;

	struct SSTableManagerOptions
	{
		std::size_t sstable_block_size = 4096;
	} sstable_manager_options;

	struct ManifestOptions
	{
		std::filesystem::path path;
	} manifest_options;
	
	struct ArenaOptions
	{
		std::size_t page_size = 64 * 1024;
		std::size_t large_threshold = 16 * 1024;
	} arena_options;

	struct LevelManagerOptions
	{
		std::size_t max_levels = 7;
	} level_manager_options;

	struct MemTableOptions
	{
		std::size_t n;
	} mem_table_options;
};