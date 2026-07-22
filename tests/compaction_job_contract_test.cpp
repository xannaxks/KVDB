#include <gtest/gtest.h>

#include <filesystem>
#include <stdexcept>
#include <string>

#include "compaction_job.h"

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
} // namespace

TEST(CompactionJobContractTest, RejectsInvalidPlanBeforeDoingIo)
{
    Arena arena;
    Manifest manifest;
    SSTableManager manager(std::filesystem::temp_directory_path());
    CompactionJob job;
    CompactionPlan invalid;

    auto result = job.run(invalid, manifest, manager, arena);
    EXPECT_FALSE(result.is_ok());
}

TEST(CompactionJobContractTest, RejectsStalePlanBeforeOpeningTable)
{
    Arena arena;
    Manifest manifest;
    SSTableManager manager(std::filesystem::temp_directory_path());
    CompactionJob job;

    CompactionPlan plan;
    plan.reason = CompactionReason::L0ReachedLimit;
    plan.source_level = 0;
    plan.target_level = 1;
    plan.smallest_key = key(arena, "a");
    plan.largest_key = key(arena, "z");

    TableMeta missing;
    missing.table_id = 123;
    missing.level = 0;
    missing.smallest_key = key(arena, "a");
    missing.largest_key = key(arena, "z");
    plan.source_tables.push_back(missing);

    ASSERT_TRUE(plan.validate());
    auto result = job.run(plan, manifest, manager, arena);
    EXPECT_FALSE(result.is_ok());
}
