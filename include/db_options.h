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
};