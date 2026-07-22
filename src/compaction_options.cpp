#include "compaction_options.h"

Status CompactionOptions::validate() const
{
    if (max_levels < 2) {
        return Status::invalid_argument("max_levels must be at least 2");
    }

    if (l0_file_count_trigger == 0) {
        return Status::invalid_argument("l0_file_count_trigger must be greater than 0");
    }

    if (max_bytes_per_level.size() < max_levels) {
        return Status::invalid_argument("max_bytes_per_level must contain one entry per level");
    }

    if (target_file_size_per_level.size() < max_levels) {
        return Status::invalid_argument("target_file_size_per_level must contain one entry per level");
    }

    // Levels 1..max_levels-2 can be selected as source levels.
    for (std::uint32_t level = 1; level + 1 < max_levels; ++level) {
        if (max_bytes_per_level[level] == 0) {
            return Status::invalid_argument(
                "max_bytes_per_level for every compactable L1+ level must be greater than 0"
            );
        }
    }

    // Levels 1..max_levels-1 can be output levels.
    for (std::uint32_t level = 1; level < max_levels; ++level) {
        if (target_file_size_per_level[level] == 0) {
            return Status::invalid_argument(
                "target_file_size_per_level for every output level must be greater than 0"
            );
        }
    }

    return Status::ok();
}
