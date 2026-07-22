#include "level_manager.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <deque>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

    // This is the only helper that may need adapting to your exact ArenaEntry
    // constructor. The strings live in a deque, so their backing storage remains
    // stable for the lifetime of each test.
    class MetaFactory {
    public:
        ArenaEntry key(std::string_view text)
        {
            storage_.emplace_back(text);
            const std::string& stored = storage_.back();
            return ArenaEntry(stored.data(), stored.size());
        }

        TableMeta table(
            std::uint64_t id,
            std::uint32_t level,
            std::string_view smallest,
            std::string_view largest,
            std::uint64_t file_size = 100
        )
        {
            TableMeta result{};
            result.table_id = id;
            result.level = level;
            result.smallest_key = key(smallest);
            result.largest_key = key(largest);
            result.file_size = file_size;
            return result;
        }

    private:
        std::deque<std::string> storage_;
    };

#define ASSERT_STATUS_OK(expression) \
    do { \
        const Status status_ = (expression); \
        ASSERT_TRUE(status_.is_ok()) << status_.message; \
    } while (false)

    TEST(LevelManagerTest, RejectsZeroConfiguredLevels)
    {
        EXPECT_THROW((void)LevelManager(0), std::invalid_argument);
    }

    TEST(LevelManagerTest, RejectsTableIdZero)
    {
        LevelManager manager(2);
        MetaFactory factory;

        Status status = manager.add_table(
            factory.table(0, 0, "a", "b")
        );

        EXPECT_FALSE(status.is_ok());
        EXPECT_EQ(status.code, StatusCode::InvalidArgument);
        EXPECT_TRUE(manager.empty());
    }

    TEST(LevelManagerTest, L0IsOrderedNewestFirst)
    {
        LevelManager manager(3);
        MetaFactory factory;

        ASSERT_STATUS_OK(manager.add_table(factory.table(3, 0, "a", "z")));
        ASSERT_STATUS_OK(manager.add_table(factory.table(7, 0, "a", "z")));
        ASSERT_STATUS_OK(manager.add_table(factory.table(5, 0, "a", "z")));

        const auto& tables = manager.levels(0);
        ASSERT_EQ(tables.size(), 3u);
        EXPECT_EQ(tables[0].table_id, 7u);
        EXPECT_EQ(tables[1].table_id, 5u);
        EXPECT_EQ(tables[2].table_id, 3u);
    }

    TEST(LevelManagerTest, RejectsDuplicateIdAcrossDifferentLevels)
    {
        LevelManager manager(3);
        MetaFactory factory;

        ASSERT_STATUS_OK(manager.add_table(factory.table(11, 0, "a", "c")));

        Status status = manager.add_table(
            factory.table(11, 2, "x", "z")
        );

        EXPECT_FALSE(status.is_ok());
        EXPECT_EQ(status.code, StatusCode::Duplicate);
        EXPECT_TRUE(manager.levels(2).empty());
    }

    TEST(LevelManagerTest, L1PlusIsSortedBySmallestKey)
    {
        LevelManager manager(3);
        MetaFactory factory;

        ASSERT_STATUS_OK(manager.add_table(factory.table(2, 1, "m", "z")));
        ASSERT_STATUS_OK(manager.add_table(factory.table(1, 1, "a", "f")));
        ASSERT_STATUS_OK(manager.add_table(factory.table(3, 1, "g", "l")));

        const auto& tables = manager.levels(1);
        ASSERT_EQ(tables.size(), 3u);
        EXPECT_EQ(tables[0].table_id, 1u);
        EXPECT_EQ(tables[1].table_id, 3u);
        EXPECT_EQ(tables[2].table_id, 2u);
    }

    TEST(LevelManagerTest, RejectsInclusiveBoundaryOverlapWithoutMutation)
    {
        LevelManager manager(2);
        MetaFactory factory;

        ASSERT_STATUS_OK(manager.add_table(factory.table(1, 1, "a", "m")));

        Status status = manager.add_table(
            factory.table(2, 1, "m", "z")
        );

        EXPECT_FALSE(status.is_ok());
        EXPECT_EQ(status.code, StatusCode::InvariantViolation);
        ASSERT_EQ(manager.levels(1).size(), 1u);
        EXPECT_EQ(manager.levels(1)[0].table_id, 1u);
    }

    TEST(LevelManagerTest, FindsAllMatchingL0TablesInNewestFirstOrder)
    {
        LevelManager manager(2);
        MetaFactory factory;

        ASSERT_STATUS_OK(manager.add_table(factory.table(4, 0, "a", "m")));
        ASSERT_STATUS_OK(manager.add_table(factory.table(9, 0, "f", "z")));
        ASSERT_STATUS_OK(manager.add_table(factory.table(6, 0, "x", "z")));

        const ArenaEntry query = factory.key("h");
        auto result = manager.find_candidate_tables_in_level(0, query);

        ASSERT_TRUE(result.is_ok()) << result.status.message;
        ASSERT_EQ(result.value.size(), 2u);
        EXPECT_EQ(result.value[0].table_id, 9u);
        EXPECT_EQ(result.value[1].table_id, 4u);
    }

    TEST(LevelManagerTest, FindsAtMostOneCandidateInNonOverlappingLevel)
    {
        LevelManager manager(2);
        MetaFactory factory;

        ASSERT_STATUS_OK(manager.add_table(factory.table(1, 1, "a", "f")));
        ASSERT_STATUS_OK(manager.add_table(factory.table(2, 1, "g", "m")));
        ASSERT_STATUS_OK(manager.add_table(factory.table(3, 1, "n", "z")));

        const ArenaEntry query = factory.key("j");
        auto result = manager.find_candidate_tables_in_level(1, query);

        ASSERT_TRUE(result.is_ok()) << result.status.message;
        ASSERT_EQ(result.value.size(), 1u);
        EXPECT_EQ(result.value[0].table_id, 2u);
    }

    TEST(LevelManagerTest, OverlapQueryUsesInclusiveRanges)
    {
        LevelManager manager(2);
        MetaFactory factory;

        ASSERT_STATUS_OK(manager.add_table(factory.table(1, 1, "a", "f")));
        ASSERT_STATUS_OK(manager.add_table(factory.table(2, 1, "g", "m")));
        ASSERT_STATUS_OK(manager.add_table(factory.table(3, 1, "n", "z")));

        const ArenaEntry smallest = factory.key("f");
        const ArenaEntry largest = factory.key("g");
        auto result = manager.find_overlapping_tables(1, smallest, largest);

        ASSERT_EQ(result.size(), 2u);
        EXPECT_EQ(result[0].table_id, 1u);
        EXPECT_EQ(result[1].table_id, 2u);
    }

    TEST(LevelManagerTest, InvalidTargetLevelDoesNotRemoveAnything)
    {
        LevelManager manager(2);
        MetaFactory factory;

        ASSERT_STATUS_OK(manager.add_table(factory.table(1, 0, "a", "b")));

        Status status = manager.remove_table(1, 9);

        EXPECT_FALSE(status.is_ok());
        EXPECT_EQ(status.code, StatusCode::InvalidArgument);
        ASSERT_EQ(manager.levels(0).size(), 1u);
    }

    TEST(LevelManagerTest, LayerSizeSumsFileSizes)
    {
        LevelManager manager(2);
        MetaFactory factory;

        ASSERT_STATUS_OK(manager.add_table(factory.table(1, 0, "a", "b", 50)));
        ASSERT_STATUS_OK(manager.add_table(factory.table(2, 0, "c", "d", 75)));

        EXPECT_EQ(manager.get_layer_size(0), 125u);
    }

} // namespace