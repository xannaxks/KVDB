#include "compaction_options.h"

Status CompactionOptions::validate() const
{
    if (max_levels < 2) {
        return Status::invalid_argument("max_levels must be at least 2");
    }

    if (max_bytes_per_level.size() < max_levels) {
        return Status::invalid_argument("max_bytes_per_level too small");
    }

    if (target_file_size_per_level.size() < max_levels) {
        return Status::invalid_argument("target_file_size_per_level too small");
    }

    return Status::ok();
}