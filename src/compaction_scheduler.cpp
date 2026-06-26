#include "compaction_scheduler.h"
#include <algorithm>
#include <cassert>

bool CompactionScheduler::should_compact(const LevelManager& manager, const CompactionOptions& options) const
{
	if (manager.levels(0).size() >= options.l0_file_count_trigger)
		return true;

	for (std::size_t level = 1; level + 1 < options.max_levels; level++)
	{
		assert(options.max_bytes_per_level.size() > level);
		if (manager.get_layer_size(level) > options.max_bytes_per_level[level])
			return true;
	}

	return false;
}

Result<std::optional<CompactionPlan>> pick_level_compaction(
	const LevelManager& manager,
	std::size_t level,
	const CompactionOptions& options
)
{
	CompactionPlan plan;

	plan.reason = CompactionReason::LxReachedLimit;

	if (level == 0)
	{
		if (manager.levels(0).empty())
			return Result<std::optional<CompactionPlan>>::fail(
				Status{
					StatusCode::InvalidState,
					"Tried to access empty 0 layer table meta"
				}
			);

		plan.reason = CompactionReason::L0ReachedLimit;

		plan.source_level = 0;
		plan.target_level = 1;

		assert(plan.target_level < options.target_file_size_per_level.size());
		plan.max_output_file_size = options.target_file_size_per_level[plan.target_level];

		const auto& tables = manager.levels(0);

		plan.smallest_key = tables.front().smallest_key;
		plan.largest_key = tables.front().largest_key;

		for (const auto& table : tables) {
			if (table.smallest_key < plan.smallest_key) {
				plan.smallest_key = table.smallest_key;
			}

			if (plan.largest_key < table.largest_key) {
				plan.largest_key = table.largest_key;
			}

			plan.source_tables.push_back(table);
		}
	}
	else
	{
		if (manager.levels(level).empty())
			return Result<std::optional<CompactionPlan>>::fail(
				Status{
					StatusCode::InvalidState,
					"Tried to access empty levels meta table"
				}
			);

		plan.reason = CompactionReason::LxReachedLimit;

		plan.source_level = level;
		plan.target_level = level + 1;

		assert(plan.target_level < options.target_file_size_per_level.size());
		plan.max_output_file_size = options.target_file_size_per_level[plan.target_level];

		plan.source_tables.emplace_back(manager.levels(level).back());

		plan.smallest_key = manager.levels(level).back().smallest_key;
		plan.largest_key = manager.levels(level).back().largest_key;
	}

	plan.overlapping_tables = std::move(manager.find_overlapping_tables(plan.target_level, plan.smallest_key, plan.largest_key));

	return Result<std::optional<CompactionPlan>>::ok(std::move(plan));
}

Result<std::optional<CompactionPlan>> CompactionScheduler::pick_compaction(
	const LevelManager& manager,
	const CompactionOptions& options)
{
	if (manager.levels(0).size() >= options.l0_file_count_trigger)
		return pick_level_compaction(manager, 0, options);

	for (std::size_t level = 1; level + 1 < options.max_levels; level++)
	{
		assert(level < options.max_bytes_per_level.size());
		if (manager.get_layer_size(level) > options.max_bytes_per_level[level])
			return pick_level_compaction(manager, level, options);
	}

	return Result<std::optional<CompactionPlan>>::ok(std::nullopt);
}