#pragma once
#include "arena.h"
#include "status.h"
#include <cstdint>
#include "table_meta.h"

enum class CompactionReason : std::uint8_t
{
	Manual,
	L0ReachedLimit,
	LxReachedLimit,
};

struct CompactionPlan
{
	CompactionReason reason;

	std::size_t source_level;
	std::size_t target_level;

	std::vector<TableMeta> source_tables;
	std::vector<TableMeta> overlapping_tables;
	
	ArenaEntry smallest_key;
	ArenaEntry largest_key;

	std::size_t max_output_file_size = 4 * 1024 * 1024;

	bool validate();

	bool validate() const;
};