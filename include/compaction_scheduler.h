#pragma once
#include "status.h"
#include "arena.h"
#include "table_meta.h"
#include "compaction_job.h"
#include "compaction_plan.h"
#include "level_manager.h"
#include "compaction_options.h"
#include <optional>

class CompactionScheduler;

class CompactionScheduler
{
public:
	bool should_compact(const LevelManager& levels, const CompactionOptions& options) const;

	Result<std::optional<CompactionPlan>> pick_compaction(
		const LevelManager& levels,
		const CompactionOptions& options
	);
};