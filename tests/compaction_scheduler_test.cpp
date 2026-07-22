#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <utility>

#include "compaction_scheduler.h"

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
    const std::string& largest,
    std::uint64_t file_size = 100
)
{
    TableMeta result;
    result.table_id = id;
    result.level = level;
    result.smallest_key = key(arena, smallest);
    result.largest_key = key(arena, largest);
    result.file_size = file_size;
    return result;
}

void add(LevelManager& manager, TableMeta value)
{
    Status status = manager.add_table(std::move(value));
    ASSERT_TRUE(status.is_ok());
}
} // namespace

TEST(CompactionSchedulerTest, DoesNothingBelowThresholds)
{
    Arena arena;
    LevelManager manager;
    CompactionOptions options;
    CompactionScheduler scheduler;

    add(manager, table(arena, 1, 0, "a", "b"));

    EXPECT_FALSE(scheduler.should_compact(manager, options));
    auto result = scheduler.pick_compaction(manager, options);
    ASSERT_TRUE(result.is_ok());
    EXPECT_FALSE(result.value.has_value());
}

TEST(CompactionSchedulerTest, L0TriggerSelectsAllL0FilesAndOverlaps)
{
    Arena arena;
    LevelManager manager;
    CompactionOptions options;
    options.l0_file_count_trigger = 2;
    CompactionScheduler scheduler;

    add(manager, table(arena, 1, 0, "d", "h"));
    add(manager, table(arena, 2, 0, "a", "e"));
    add(manager, table(arena, 3, 1, "c", "f"));
    add(manager, table(arena, 4, 1, "x", "z"));

    ASSERT_TRUE(scheduler.should_compact(manager, options));
    auto result = scheduler.pick_compaction(manager, options);
    ASSERT_TRUE(result.is_ok());
    ASSERT_TRUE(result.value.has_value());

    const CompactionPlan& plan = *result.value;
    EXPECT_EQ(plan.reason, CompactionReason::L0ReachedLimit);
    EXPECT_EQ(plan.source_level, 0u);
    EXPECT_EQ(plan.target_level, 1u);
    EXPECT_EQ(plan.source_tables.size(), 2u);
    ASSERT_EQ(plan.overlapping_tables.size(), 1u);
    EXPECT_EQ(plan.overlapping_tables.front().table_id, 3u);
    EXPECT_TRUE(plan.validate());
}

TEST(CompactionSchedulerTest, L1SizeTriggerSelectsLargestTable)
{
    Arena arena;
    LevelManager manager;
    CompactionOptions options;
    options.max_levels = 3;
    options.max_bytes_per_level = {0, 100, 0};
    options.target_file_size_per_level = {1, 10, 20};
    CompactionScheduler scheduler;

    add(manager, table(arena, 10, 1, "a", "c", 60));
    add(manager, table(arena, 11, 1, "d", "f", 70));

    auto result = scheduler.pick_compaction(manager, options);
    ASSERT_TRUE(result.is_ok());
    ASSERT_TRUE(result.value.has_value());

    const CompactionPlan& plan = *result.value;
    EXPECT_EQ(plan.reason, CompactionReason::LxReachedLimit);
    ASSERT_EQ(plan.source_tables.size(), 1u);
    EXPECT_EQ(plan.source_tables.front().table_id, 11u);
    EXPECT_EQ(plan.target_level, 2u);
    EXPECT_EQ(plan.max_output_file_size, 20u);
}

TEST(CompactionSchedulerTest, RejectsInvalidOptions)
{
    LevelManager manager;
    CompactionOptions options;
    options.l0_file_count_trigger = 0;
    CompactionScheduler scheduler;

    EXPECT_FALSE(scheduler.should_compact(manager, options));
    auto result = scheduler.pick_compaction(manager, options);
    EXPECT_FALSE(result.is_ok());
}
