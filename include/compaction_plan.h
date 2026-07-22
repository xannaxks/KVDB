#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "arena.h"
#include "table_meta.h"

enum class CompactionReason : std::uint8_t
{
    Manual,
    L0ReachedLimit,
    LxReachedLimit,
};

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

    [[nodiscard]] bool validate() const;
};
