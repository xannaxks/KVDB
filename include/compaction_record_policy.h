/**
 * @file compaction_record_policy.h
 * @brief Version-retention rule used during sorted compaction merge.
 */
#pragma once

#include "type.h"

// This policy assumes KVDB has no active snapshots. Once snapshot reads are
// introduced, compaction must retain versions required by the oldest snapshot.
/**
 * @brief Returns true for the first (newest) record of each user key.
 *
 * Inputs must arrive in internal-key order: key ascending, sequence descending.
 */
[[nodiscard]] inline bool compaction_keep_newest_record(
    ::Type type,
    bool is_bottommost_level
) noexcept
{
    return !(type == ::Type::Tombstone && is_bottommost_level);
}
