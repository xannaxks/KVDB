#pragma once

#include <optional>

#include "compaction_options.h"
#include "compaction_plan.h"
#include "level_manager.h"
#include "status.h"

class CompactionScheduler
{
public:
    [[nodiscard]] bool should_compact(
        const LevelManager& levels,
        const CompactionOptions& options
    ) const;

    [[nodiscard]] Result<std::optional<CompactionPlan>> pick_compaction(
        const LevelManager& levels,
        const CompactionOptions& options
    ) const;
};
