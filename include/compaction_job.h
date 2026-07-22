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
        const Manifest& manifest,
        SSTableManager& sstable_manager,
        Arena& arena
    ) const;
};
