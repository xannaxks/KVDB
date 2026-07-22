#include "compaction_scheduler.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace
{
    const std::vector<TableMeta>* tables_at(
        const LevelManager& manager,
        std::uint32_t level
    ) noexcept
    {
        return manager.get_lx_tables(level);
    }

    std::uint64_t level_size_bytes(
        const LevelManager& manager,
        std::uint32_t level
    ) noexcept
    {
        const auto* tables = tables_at(manager, level);
        if (tables == nullptr) {
            return 0;
        }

        std::uint64_t total = 0;
        for (const auto& table : *tables) {
            if (table.file_size > std::numeric_limits<std::uint64_t>::max() - total) {
                return std::numeric_limits<std::uint64_t>::max();
            }
            total += table.file_size;
        }
        return total;
    }

    void widen_plan_range(CompactionPlan& plan, const TableMeta& table)
    {
        if (table.smallest_key < plan.smallest_key) {
            plan.smallest_key = table.smallest_key;
        }
        if (plan.largest_key < table.largest_key) {
            plan.largest_key = table.largest_key;
        }
    }

    Result<std::optional<CompactionPlan>> pick_level_compaction(
        const LevelManager& manager,
        std::uint32_t level,
        const CompactionOptions& options
    )
    {
        const auto* source = tables_at(manager, level);
        if (source == nullptr || source->empty()) {
            return Result<std::optional<CompactionPlan>>::fail(
                Status{
                    StatusCode::InvalidState,
                    "Compaction source level is missing or empty"
                }
            );
        }

        if (level + 1 >= options.max_levels) {
            return Result<std::optional<CompactionPlan>>::fail(
                Status{
                    StatusCode::InvalidState,
                    "Cannot compact the bottommost level"
                }
            );
        }

        CompactionPlan plan;
        plan.source_level = level;
        plan.target_level = level + 1;
        plan.max_output_file_size =
            options.target_file_size_per_level[plan.target_level];

        if (level == 0) {
            plan.reason = CompactionReason::L0ReachedLimit;
            plan.source_tables = *source;
        }
        else {
            plan.reason = CompactionReason::LxReachedLimit;

            // Without a persisted compaction pointer, selecting the largest table
            // is a better default than always selecting the rightmost key range.
            const auto selected = std::max_element(
                source->begin(),
                source->end(),
                [](const TableMeta& lhs, const TableMeta& rhs) {
                    if (lhs.file_size != rhs.file_size) {
                        return lhs.file_size < rhs.file_size;
                    }
                    return lhs.table_id > rhs.table_id;
                }
            );
            plan.source_tables.push_back(*selected);
        }

        plan.smallest_key = plan.source_tables.front().smallest_key;
        plan.largest_key = plan.source_tables.front().largest_key;

        for (const auto& table : plan.source_tables) {
            widen_plan_range(plan, table);
        }

        plan.overlapping_tables = manager.find_overlapping_tables(
            plan.target_level,
            plan.smallest_key,
            plan.largest_key
        );

        for (const auto& table : plan.overlapping_tables) {
            widen_plan_range(plan, table);
        }

        if (!plan.validate()) {
            return Result<std::optional<CompactionPlan>>::fail(
                Status{
                    StatusCode::Corruption,
                    "Scheduler produced an invalid compaction plan"
                }
            );
        }

        return Result<std::optional<CompactionPlan>>::ok(std::move(plan));
    }
} // namespace

bool CompactionScheduler::should_compact(
    const LevelManager& manager,
    const CompactionOptions& options
) const
{
    if (!options.validate().is_ok()) {
        return false;
    }

    const auto* l0 = tables_at(manager, 0);
    if (l0 != nullptr && l0->size() >= options.l0_file_count_trigger) {
        return true;
    }

    for (std::uint32_t level = 1; level + 1 < options.max_levels; ++level) {
        if (level_size_bytes(manager, level) > options.max_bytes_per_level[level]) {
            return true;
        }
    }

    return false;
}

Result<std::optional<CompactionPlan>> CompactionScheduler::pick_compaction(
    const LevelManager& manager,
    const CompactionOptions& options
) const
{
    Status options_status = options.validate();
    if (!options_status.is_ok()) {
        return Result<std::optional<CompactionPlan>>::fail(
            std::move(options_status)
        );
    }

    const auto* l0 = tables_at(manager, 0);
    if (l0 != nullptr && l0->size() >= options.l0_file_count_trigger) {
        return pick_level_compaction(manager, 0, options);
    }

    for (std::uint32_t level = 1; level + 1 < options.max_levels; ++level) {
        if (level_size_bytes(manager, level) > options.max_bytes_per_level[level]) {
            return pick_level_compaction(manager, level, options);
        }
    }

    return Result<std::optional<CompactionPlan>>::ok(std::nullopt);
}
