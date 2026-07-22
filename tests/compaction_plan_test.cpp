#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

#include "compaction_plan.h"

namespace
{
ArenaEntry key(Arena& arena, const std::string& text)
{
    auto result = ArenaEntry::make_entry(arena, text);
    if (!result.is_ok()) {
        throw std::runtime_error("ArenaEntry allocation failed");
    }
    return result.value;
}

TableMeta table(
    Arena& arena,
    std::uint64_t id,
    std::uint32_t level,
    const std::string& smallest,
    const std::string& largest
)
{
    TableMeta result;
    result.table_id = id;
    result.level = level;
    result.smallest_key = key(arena, smallest);
    result.largest_key = key(arena, largest);
    result.file_size = 100;
    return result;
}

CompactionPlan valid_plan(Arena& arena)
{
    CompactionPlan plan;
    plan.reason = CompactionReason::L0ReachedLimit;
    plan.source_level = 0;
    plan.target_level = 1;
    plan.smallest_key = key(arena, "a");
    plan.largest_key = key(arena, "z");
    plan.source_tables.push_back(table(arena, 1, 0, "b", "m"));
    plan.overlapping_tables.push_back(table(arena, 2, 1, "m", "y"));
    return plan;
}
} // namespace

TEST(CompactionPlanTest, AcceptsValidPlan)
{
    Arena arena;
    EXPECT_TRUE(valid_plan(arena).validate());
}

TEST(CompactionPlanTest, RejectsEmptySource)
{
    Arena arena;
    auto plan = valid_plan(arena);
    plan.source_tables.clear();
    EXPECT_FALSE(plan.validate());
}

TEST(CompactionPlanTest, RejectsNonAdjacentLevels)
{
    Arena arena;
    auto plan = valid_plan(arena);
    plan.target_level = 2;
    EXPECT_FALSE(plan.validate());
}

TEST(CompactionPlanTest, RejectsDuplicateTableIds)
{
    Arena arena;
    auto plan = valid_plan(arena);
    plan.overlapping_tables.front().table_id =
        plan.source_tables.front().table_id;
    EXPECT_FALSE(plan.validate());
}

TEST(CompactionPlanTest, RejectsWrongTableLevel)
{
    Arena arena;
    auto plan = valid_plan(arena);
    plan.source_tables.front().level = 1;
    EXPECT_FALSE(plan.validate());
}

TEST(CompactionPlanTest, RejectsTableOutsidePlanRange)
{
    Arena arena;
    auto plan = valid_plan(arena);
    plan.source_tables.front().smallest_key = key(arena, "0");
    EXPECT_FALSE(plan.validate());
}

TEST(CompactionPlanTest, RejectsZeroOutputSize)
{
    Arena arena;
    auto plan = valid_plan(arena);
    plan.max_output_file_size = 0;
    EXPECT_FALSE(plan.validate());
}
