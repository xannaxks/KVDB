#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <utility>

#include "level_manager.h"

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
    TableMeta value;
    value.table_id = id;
    value.level = level;
    value.smallest_key = key(arena, smallest);
    value.largest_key = key(arena, largest);
    return value;
}
} // namespace

TEST(LevelManagerCompactionRegressionTest, AddsFirstTableToEmptyL1)
{
    Arena arena;
    LevelManager manager;

    Status status = manager.add_table(table(arena, 1, 1, "a", "m"));
    EXPECT_TRUE(status.is_ok());
}

TEST(LevelManagerCompactionRegressionTest, RejectsOverlapOnEitherSide)
{
    Arena arena;
    LevelManager manager;

    ASSERT_TRUE(manager.add_table(table(arena, 1, 1, "d", "f")).is_ok());
    EXPECT_FALSE(manager.add_table(table(arena, 2, 1, "a", "d")).is_ok());
    EXPECT_FALSE(manager.add_table(table(arena, 3, 1, "f", "z")).is_ok());
}

TEST(LevelManagerCompactionRegressionTest,ResizesDirectlyToRequestedLevel)
{
    Arena arena;
    LevelManager manager;

    Status status = manager.add_table(table(arena, 1, 4, "a", "z"));
    ASSERT_TRUE(status.is_ok());
    ASSERT_NE(manager.get_lx_tables(4), nullptr);
    EXPECT_EQ(manager.get_lx_tables(4)->size(), 1u);
}
