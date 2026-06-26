#pragma once
#include <cstdint>
#include <vector>
#include "status.h"

struct CompactionOptions
{
    std::uint32_t max_levels = 7;

    // L0 is special because files can overlap.
    std::uint32_t l0_file_count_trigger = 4;

    // For L1+ compaction trigger.
    std::vector<std::uint64_t> max_bytes_per_level = {
        0,                              // L0 ignored here
        64ull * 1024 * 1024,            // L1
        640ull * 1024 * 1024,           // L2
        6400ull * 1024 * 1024           // L3
    };

    // Output SSTable target sizes.
    std::vector<std::uint64_t> target_file_size_per_level = {
        8ull * 1024 * 1024,             // L0, mostly unused
        8ull * 1024 * 1024,             // L1
        16ull * 1024 * 1024,            // L2
        32ull * 1024 * 1024             // L3
    };

    Status validate() const;

};