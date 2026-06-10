#include "level_manager.h"
#include <utility>
#include <algorithm>
#include <format>

LevelManager::LevelManager() noexcept
{
	this->levels_.resize(2);
}

Status LevelManager::add_table(TableMeta&& meta)
{
	if (meta.level >= levels_.size())
	{
		levels_.emplace_back(std::vector<TableMeta>());
	}

	auto& level_tables = levels_[meta.level];

	if (meta.level == 0)
	{
		auto it = std::lower_bound(
			level_tables.begin(),
			level_tables.end(),
			meta.table_id,
			[](const TableMeta& other, std::uint64_t current)
			{
				return other.table_id > current;
			}
		);

		level_tables.insert(it, std::move(meta));
		return Status::ok();
	}

	auto it = std::lower_bound(
		level_tables.begin(),
		level_tables.end(),
		meta.smallest_key,
		[](const TableMeta& other, const ArenaEntry& current)
		{
			return other.smallest_key < current;
		}
	);

	if (it != level_tables.begin())
	{
		auto prev = it - 1;
		if ((*prev).largest_key > meta.smallest_key or (*prev).largest_key == meta.smallest_key)
			return Status{ StatusCode::OverlappingKeys, "Level table overlaps with previous table" };
	}
	if (it != --level_tables.end())
	{
		auto next = it + 1;
		if (meta.largest_key > (*next).smallest_key or (*next).smallest_key == meta.largest_key)
			return Status{ StatusCode::OverlappingKeys, "Level table overlaps with next table" };
	}

	level_tables.insert(it, std::move(meta));
	return Status::ok();
}

Status LevelManager::remove_table(
	std::uint64_t table_id,
	std::optional<std::uint32_t> target_level
)
{
	auto remove_from_level = [&](std::uint32_t level_index) -> Status {
		if (level_index >= levels_.size()) {
			return Status{
				StatusCode::NotFound,
				std::format("level {} does not exist while removing table {}", level_index, table_id)
			};
		}

		auto& tables = levels_[level_index];

		for (auto it = tables.begin(); it != tables.end(); ++it) {
			if (it->table_id == table_id) {
				tables.erase(it);
				return Status::ok();
			}
		}

		return Status{
			StatusCode::NotFound,
			std::format("could not find table id {} on level {}", table_id, level_index)
		};
		};

	if (target_level.has_value()) {
		return remove_from_level(*target_level);
	}

	for (std::uint32_t level_index = 0; level_index < levels_.size(); ++level_index) {
		Status result = remove_from_level(level_index);

		if (result.is_ok())
			return Status::ok();
	}

	return Status{
		StatusCode::NotFound,
		std::format("could not find table id {} in any level", table_id)
	};
}

const std::vector<TableMeta>* LevelManager::get_lx_tables(std::uint32_t level) const
{
	if (level >= this->levels_.size()) return nullptr;
	return &(this->levels_[level]);
}

Result<std::vector<TableMeta>> LevelManager::find_candidate_tables_in_level(
	std::uint32_t level,
	const ArenaEntry& key
) const
{
	if (level >= levels_.size()) {
		return Result<std::vector<TableMeta>>::fail(
			Status{
				StatusCode::NotFound,
				std::format("level {} does not exist", level)
			}
		);
	}

	const auto& tables = levels_[level];

	if (tables.empty()) {
		return Result<std::vector<TableMeta>>::ok({});
	}

	std::vector<TableMeta> result;

	if (level == 0) {
		for (const auto& table : tables) {
			const bool key_is_before_table = key < table.smallest_key;
			const bool key_is_after_table = table.largest_key < key;

			if (!key_is_before_table && !key_is_after_table) {
				result.push_back(table);
			}
		}

		return Result<std::vector<TableMeta>>::ok(std::move(result));
	}

	auto it = std::lower_bound(
		tables.begin(),
		tables.end(),
		key,
		[](const TableMeta& table, const ArenaEntry& key)
		{
			return table.largest_key < key;
		}
	);

	if (it == tables.end()) {
		return Result<std::vector<TableMeta>>::ok({});
	}

	const bool key_is_before_table = key < it->smallest_key;
	const bool key_is_after_table = it->largest_key < key;

	if (!key_is_before_table && !key_is_after_table) {
		result.push_back(*it);
	}

	return Result<std::vector<TableMeta>>::ok(std::move(result));
}

std::vector<TableMeta> LevelManager::find_overlapping_tables(
	std::uint32_t level,
	const ArenaEntry& smallest,
	const ArenaEntry& largest
) const
{
	std::vector<TableMeta> result;

	if (level >= levels_.size()) {
		return result; 
	}

	if (largest < smallest) {
		return result; 
	}

	const auto& level_tables = levels_[level];

	if (level == 0) {
		for (const auto& table : level_tables) {
			const bool table_is_before_range = table.largest_key < smallest;
			const bool table_is_after_range = largest < table.smallest_key;

			if (!table_is_before_range && !table_is_after_range) {
				result.push_back(table);
			}
		}

		return result;
	}

	auto it = std::lower_bound(
		level_tables.begin(),
		level_tables.end(),
		smallest,
		[](const TableMeta& table, const ArenaEntry& key) {
			return table.largest_key < key;
		}
	);

	for (; it != level_tables.end(); ++it) {
		if (largest < it->smallest_key) {
			break;
		}

		result.push_back(*it);
	}

	return result;
}