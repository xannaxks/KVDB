#pragma once

#include <optional>

#include "arena.h"
#include "compaction_plan.h"
#include "manifest.h"
#include "sstable_manager.h"
#include "status.h"

class CompactionJob
{
public:
    [[nodiscard]] Result<std::optional<VersionEdit>> run(
        const CompactionPlan& plan,
        const LevelManager& level_manager,
        const Manifest& manifest,
        SSTableManager& sstable_manager,
        Arena& arena
    ) const;

    // Compatibility overload for callers that only need plan validation.
    // A plan containing table inputs is necessarily stale without a level
    // snapshot and is rejected before any I/O.
    [[nodiscard]] Result<std::optional<VersionEdit>> run(
        const CompactionPlan& plan,
        const Manifest& manifest,
        SSTableManager& sstable_manager,
        Arena& arena
    ) const;
};
