/**
 * @file compaction_job.h
 * @brief Executes a validated compaction plan and prepares its manifest edit.
 */
#pragma once

#include <optional>

#include "arena.h"
#include "compaction_plan.h"
#include "manifest.h"
#include "sstable_manager.h"
#include "status.h"
/**
 * @brief Merges selected SSTables into replacement tables.
 *
 * A job verifies that the plan still matches the supplied level snapshot,
 * opens one iterator per input, performs a k-way merge, drops superseded
 * versions according to compaction policy, and builds output SSTables. It
 * returns a VersionEdit describing the files to add and delete; publishing
 * that edit remains the Engine/Manifest's responsibility.
 */
class CompactionJob
{
public:
    /**
     * @brief Runs one compaction against a current level snapshot.
     * @return Empty when the plan has no work, otherwise an uncommitted edit.
     * @callgraph
     */
    [[nodiscard]] Result<std::optional<VersionEdit>> run(
        const CompactionPlan& plan,
        const LevelManager& level_manager,
        const Manifest& manifest,
        SSTableManager& sstable_manager,
        Arena& arena
    ) const;

    /**
     * @brief Compatibility overload for validation-only callers.
     *
     * A plan containing inputs cannot be freshness-checked without a level
     * snapshot and is rejected before performing I/O.
     */
    [[nodiscard]] Result<std::optional<VersionEdit>> run(
        const CompactionPlan& plan,
        const Manifest& manifest,
        SSTableManager& sstable_manager,
        Arena& arena
    ) const;
};
