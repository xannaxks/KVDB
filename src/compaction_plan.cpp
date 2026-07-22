#include "compaction_plan.h"

#include <limits>
#include <unordered_set>

namespace
{
    bool table_range_is_valid(const TableMeta& table) noexcept
    {
        return !(table.largest_key < table.smallest_key);
    }

    bool range_contains(
        const ArenaEntry& plan_smallest,
        const ArenaEntry& plan_largest,
        const TableMeta& table
    ) noexcept
    {
        return !(table.smallest_key < plan_smallest) &&
            !(plan_largest < table.largest_key);
    }
} // namespace

bool CompactionPlan::validate() const
{
    if (source_tables.empty()) {
        return false;
    }

    if (source_level == std::numeric_limits<std::uint32_t>::max() ||
        target_level != source_level + 1) {
        return false;
    }

    if (max_output_file_size == 0) {
        return false;
    }

    if (largest_key < smallest_key) {
        return false;
    }

    if (reason == CompactionReason::L0ReachedLimit && source_level != 0) {
        return false;
    }

    if (reason == CompactionReason::LxReachedLimit && source_level == 0) {
        return false;
    }

    std::unordered_set<std::uint64_t> table_ids;
    table_ids.reserve(source_tables.size() + overlapping_tables.size());

    const auto validate_table = [&](const TableMeta& table, std::uint32_t expected_level) {
        if (table.table_id == 0 || table.level != expected_level) {
            return false;
        }

        if (!table_range_is_valid(table) ||
            !range_contains(smallest_key, largest_key, table)) {
            return false;
        }

        return table_ids.insert(table.table_id).second;
        };

    for (const auto& table : source_tables) {
        if (!validate_table(table, source_level)) {
            return false;
        }
    }

    for (const auto& table : overlapping_tables) {
        if (!validate_table(table, target_level)) {
            return false;
        }
    }

    return true;
}
