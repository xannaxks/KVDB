#pragma once

#include <cstdint>
#include <vector>

#include "status.h"

struct CompactionOptions
{
    std::uint32_t max_levels = 7;

    // L0 is special because its files may overlap.
    std::uint32_t l0_file_count_trigger = 4;

    // L1..L(max_levels-2) limits. The bottommost level is not compacted
    // further, so its entry is retained only to keep indexing simple.
    std::vector<std::uint64_t> max_bytes_per_level = {
        0,
        64ull * 1024 * 1024,
        640ull * 1024 * 1024,
        6400ull * 1024 * 1024,
        64000ull * 1024 * 1024,
        640000ull * 1024 * 1024,
        0
    };

    // Target output SSTable sizes for each destination level.
    std::vector<std::uint64_t> target_file_size_per_level = {
        8ull * 1024 * 1024,
        8ull * 1024 * 1024,
        16ull * 1024 * 1024,
        32ull * 1024 * 1024,
        64ull * 1024 * 1024,
        128ull * 1024 * 1024,
        256ull * 1024 * 1024
    };

    [[nodiscard]] Status validate() const;
};
