#include "level_manager.h"

#include <algorithm>
#include <format>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

    [[nodiscard]] bool key_is_inside_table(
        const ArenaEntry& key,
        const TableMeta& table
    )
    {
        return !(key < table.smallest_key) &&
            !(table.largest_key < key);
    }

    [[nodiscard]] bool ranges_overlap(
        const ArenaEntry& first_smallest,
        const ArenaEntry& first_largest,
        const ArenaEntry& second_smallest,
        const ArenaEntry& second_largest
    )
    {
        return !(first_largest < second_smallest) &&
            !(second_largest < first_smallest);
    }

} // namespace

LevelManager::LevelManager(std::uint32_t level_count)
    : levels_(level_count)
{
    if (level_count == 0) {
        throw std::invalid_argument(
            "LevelManager requires at least one level"
        );
    }
}

Status LevelManager::add_table(TableMeta&& meta)
{
    if (meta.table_id == 0) {
        return Status{
            StatusCode::InvalidArgument,
            "table id zero is reserved and cannot be installed"
        };
    }

    if (meta.level >= levels_.size()) {
        return Status{
            StatusCode::InvalidArgument,
            std::format(
                "table {} targets invalid level {}; configured level count is {}",
                meta.table_id,
                meta.level,
                levels_.size()
            )
        };
    }

    if (meta.largest_key < meta.smallest_key) {
        return Status{
            StatusCode::InvalidArgument,
            std::format(
                "table {} has an invalid key range",
                meta.table_id
            )
        };
    }

    // IDs are database-wide identities, not per-level identities.
    for (std::size_t level_index = 0;
        level_index < levels_.size();
        ++level_index) {
        const auto& tables = levels_[level_index];
        const auto duplicate = std::find_if(
            tables.begin(),
            tables.end(),
            [&](const TableMeta& table) {
                return table.table_id == meta.table_id;
            }
        );

        if (duplicate != tables.end()) {
            return Status{
                StatusCode::Duplicate,
                std::format(
                    "table id {} already exists on level {}",
                    meta.table_id,
                    level_index
                )
            };
        }
    }

    auto& level_tables = levels_[meta.level];

    if (meta.level == 0) {
        // L0 may overlap. Newer files must be searched first. This ordering
        // relies on monotonically allocated, never-reused table IDs.
        const auto insertion_position = std::lower_bound(
            level_tables.begin(),
            level_tables.end(),
            meta.table_id,
            [](const TableMeta& table, std::uint64_t table_id) {
                return table.table_id > table_id;
            }
        );

        level_tables.insert(insertion_position, std::move(meta));
        return Status::ok();
    }

    // L1+ is sorted by smallest key and contains disjoint inclusive ranges.
    const auto insertion_position = std::lower_bound(
        level_tables.begin(),
        level_tables.end(),
        meta.smallest_key,
        [](const TableMeta& table, const ArenaEntry& key) {
            return table.smallest_key < key;
        }
    );

    if (insertion_position != level_tables.begin()) {
        const auto& previous = *std::prev(insertion_position);
        if (ranges_overlap(
            previous.smallest_key,
            previous.largest_key,
            meta.smallest_key,
            meta.largest_key)) {
            return Status{
                StatusCode::InvariantViolation,
                std::format(
                    "table {} overlaps previous table {} on level {}",
                    meta.table_id,
                    previous.table_id,
                    meta.level
                )
            };
        }
    }

    if (insertion_position != level_tables.end()) {
        const auto& next = *insertion_position;
        if (ranges_overlap(
            meta.smallest_key,
            meta.largest_key,
            next.smallest_key,
            next.largest_key)) {
            return Status{
                StatusCode::InvariantViolation,
                std::format(
                    "table {} overlaps next table {} on level {}",
                    meta.table_id,
                    next.table_id,
                    meta.level
                )
            };
        }
    }

    level_tables.insert(insertion_position, std::move(meta));
    return Status::ok();
}

Status LevelManager::remove_table(
    std::uint64_t table_id,
    std::optional<std::uint32_t> target_level
)
{
    if (table_id == 0) {
        return Status{
            StatusCode::InvalidArgument,
            "table id zero is invalid"
        };
    }

    if (target_level.has_value()) {
        const std::uint32_t level_index = *target_level;
        if (level_index >= levels_.size()) {
            return Status{
                StatusCode::InvalidArgument,
                std::format(
                    "level {} does not exist while removing table {}",
                    level_index,
                    table_id
                )
            };
        }

        auto& tables = levels_[level_index];
        const auto first_match = std::find_if(
            tables.begin(),
            tables.end(),
            [&](const TableMeta& table) {
                return table.table_id == table_id;
            }
        );

        if (first_match == tables.end()) {
            return Status{
                StatusCode::NotFound,
                std::format(
                    "could not find table id {} on level {}",
                    table_id,
                    level_index
                )
            };
        }

        const auto second_match = std::find_if(
            std::next(first_match),
            tables.end(),
            [&](const TableMeta& table) {
                return table.table_id == table_id;
            }
        );

        if (second_match != tables.end()) {
            return Status{
                StatusCode::InvariantViolation,
                std::format(
                    "table id {} appears multiple times on level {}",
                    table_id,
                    level_index
                )
            };
        }

        tables.erase(first_match);
        return Status::ok();
    }

    std::optional<std::pair<std::size_t, std::size_t>> found_location;

    for (std::size_t level_index = 0;
        level_index < levels_.size();
        ++level_index) {
        const auto& tables = levels_[level_index];
        for (std::size_t table_index = 0;
            table_index < tables.size();
            ++table_index) {
            if (tables[table_index].table_id != table_id) {
                continue;
            }

            if (found_location.has_value()) {
                return Status{
                    StatusCode::InvariantViolation,
                    std::format(
                        "table id {} appears more than once in LevelManager",
                        table_id
                    )
                };
            }

            found_location = std::pair{ level_index, table_index };
        }
    }

    if (!found_location.has_value()) {
        return Status{
            StatusCode::NotFound,
            std::format(
                "could not find table id {} in any level",
                table_id
            )
        };
    }

    const auto [level_index, table_index] = *found_location;
    auto& tables = levels_[level_index];
    tables.erase(
        tables.begin() +
        static_cast<std::vector<TableMeta>::difference_type>(table_index)
    );

    return Status::ok();
}

const std::vector<TableMeta>* LevelManager::get_lx_tables(
    std::uint32_t level
) const noexcept
{
    if (level >= levels_.size()) {
        return nullptr;
    }
    return &levels_[level];
}

std::uint32_t LevelManager::level_count() const noexcept
{
    return static_cast<std::uint32_t>(levels_.size());
}

bool LevelManager::empty() const noexcept
{
    return std::all_of(
        levels_.begin(),
        levels_.end(),
        [](const auto& tables) { return tables.empty(); }
    );
}

Result<std::vector<TableMeta>>
LevelManager::find_candidate_tables_in_level(
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
    std::vector<TableMeta> result;

    if (level == 0) {
        result.reserve(tables.size());
        for (const auto& table : tables) {
            if (key_is_inside_table(key, table)) {
                result.push_back(table);
            }
        }
        return Result<std::vector<TableMeta>>::ok(std::move(result));
    }

    const auto candidate = std::lower_bound(
        tables.begin(),
        tables.end(),
        key,
        [](const TableMeta& table, const ArenaEntry& target_key) {
            return table.largest_key < target_key;
        }
    );

    if (candidate != tables.end() &&
        key_is_inside_table(key, *candidate)) {
        result.push_back(*candidate);
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

    if (level >= levels_.size() || largest < smallest) {
        return result;
    }

    const auto& level_tables = levels_[level];

    if (level == 0) {
        result.reserve(level_tables.size());
        for (const auto& table : level_tables) {
            if (ranges_overlap(
                table.smallest_key,
                table.largest_key,
                smallest,
                largest)) {
                result.push_back(table);
            }
        }
        return result;
    }

    auto current = std::lower_bound(
        level_tables.begin(),
        level_tables.end(),
        smallest,
        [](const TableMeta& table, const ArenaEntry& key) {
            return table.largest_key < key;
        }
    );

    for (; current != level_tables.end(); ++current) {
        if (largest < current->smallest_key) {
            break;
        }
        result.push_back(*current);
    }

    return result;
}

const std::vector<TableMeta>& LevelManager::levels(
    std::size_t level
) const
{
    return levels_.at(level);
}

std::size_t LevelManager::get_layer_size(
    std::size_t level
) const
{
    const auto& tables = levels_.at(level);
    std::size_t result = 0;

    for (const auto& table_meta : tables) {
        if (table_meta.file_size >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max() - result)) {
            throw std::overflow_error(
                "LevelManager level size overflow"
            );
        }

        result += static_cast<std::size_t>(table_meta.file_size);
    }

    return result;
}

void LevelManager::swap(LevelManager& other) noexcept
{
    levels_.swap(other.levels_);
}