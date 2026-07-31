/**
 * @file compaction_scheduler.h
 * @brief Selects level compactions from current table metadata.
 */
#pragma once

#include <optional>

#include "compaction_options.h"
#include "compaction_plan.h"
#include "level_manager.h"
#include "status.h"

/**
 * @brief Stateless policy object that turns level pressure into a plan.
 *
 * The scheduler prioritizes L0 pressure, then scans higher compactable levels.
 * It expands a chosen source range with every overlapping table in the
 * destination level so the resulting plan preserves non-overlap invariants.
 */
class CompactionScheduler
{
public:
    /** @brief Returns whether any level currently exceeds its trigger. */
    [[nodiscard]] bool should_compact(
        const LevelManager& levels,
        const CompactionOptions& options
    ) const;

    /**
     * @brief Selects and validates the next compaction, if one is needed.
     * @callgraph
     */
    [[nodiscard]] Result<std::optional<CompactionPlan>> pick_compaction(
        const LevelManager& levels,
        const CompactionOptions& options
    ) const;
};
