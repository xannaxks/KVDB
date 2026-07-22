#pragma once

#include "type.h"

// This policy assumes KVDB has no active snapshots. Once snapshot reads are
// introduced, compaction must retain versions required by the oldest snapshot.
[[nodiscard]] inline bool compaction_keep_newest_record(
    ::Type type,
    bool is_bottommost_level
) noexcept
{
    return !(type == ::Type::Tombstone && is_bottommost_level);
}
