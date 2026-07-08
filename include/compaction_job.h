#pragma once
#include "compaction_plan.h"
#include "status.h"
#include "sstable_manager.h"
#include "manifest.h"
#include "level_manager.h"
#include "engine.h"
#include "arena.h"

class CompactionJob
{
public:
    Result<std::optional<VersionEdit>> run(
        const CompactionPlan& plan,
        LevelManager& level_manager,
        SSTableManager& sstable_manager,
        Arena& arena
    );
};