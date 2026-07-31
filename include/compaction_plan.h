/**
 * @file compaction_plan.h
 * @brief Immutable description of one level-to-level compaction.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "arena.h"
#include "table_meta.h"

/** @brief Trigger that caused the scheduler to create a compaction plan. */
enum class CompactionReason : std::uint8_t
{
    Manual, ///< Explicit user-requested range compaction.
    L0ReachedLimit, ///< Level zero exceeded its file-count trigger.
    LxReachedLimit, ///< A higher level exceeded its byte budget.
};

/**
 * @brief Complete input selection and output policy for a compaction job.
 *
 * Source tables and overlapping target-level tables form a single sorted merge.
 * The smallest/largest keys describe the selected user-key interval and the
 * output size controls when the job rolls to another SSTable.
 */
struct CompactionPlan
{
    CompactionReason reason = CompactionReason::Manual;

    std::uint32_t source_level = 0;
    std::uint32_t target_level = 0;

    std::vector<TableMeta> source_tables;
    std::vector<TableMeta> overlapping_tables;

    ArenaEntry smallest_key{};
    ArenaEntry largest_key{};

    std::uint64_t max_output_file_size = 4ull * 1024 * 1024;

    /** @brief Checks levels, inputs, key bounds, and output-size invariants. */
    [[nodiscard]] bool validate() const;
};
