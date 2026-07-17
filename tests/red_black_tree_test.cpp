#include <gtest/gtest.h>

#include "arena.h"
#include "red_black_tree.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

    template <typename>
    inline constexpr bool dependent_false_v = false;

    // Supports the common Status APIs used across this project without coupling the
    // tests to whether the code is exposed as a field or an accessor.
    template <typename StatusLike>
    StatusCode status_code_of(const StatusLike& status)
    {
        if constexpr (requires { status.code; })
            return status.code;
        else if constexpr (requires { status.code(); })
            return status.code();
        else if constexpr (requires { status.get_code(); })
            return status.get_code();
        else if constexpr (requires { status.status_code; })
            return status.status_code;
        else if constexpr (requires { status.status_code(); })
            return status.status_code();
        else
            static_assert(dependent_false_v<StatusLike>,
                "Update status_code_of() to match the Status code accessor.");
    }

    std::string entry_bytes(const ArenaEntry& entry)
    {
        return std::string(
            reinterpret_cast<const char*>(entry.data),
            entry.size
        );
    }

    std::string ordered_key(int value)
    {
        std::ostringstream out;
        out << "key-" << std::setw(6) << std::setfill('0') << value;
        return out.str();
    }

    class RBTreeTest : public ::testing::Test
    {
    protected:
        Arena arena;
        RBTree tree;

        Result<ArenaEntry> make_entry(const std::string& bytes)
        {
            Result<ArenaEntry> result = ArenaEntry::make_entry(arena, bytes);
            return result;
        }

        Result<InternalRecord> make_record(
            const std::string& key,
            const std::string& value,
            Type type,
            std::uint64_t sequence)
        {
            Result<ArenaEntry> key_result = make_entry(key);
            if (!key_result.is_ok())
                return Result<InternalRecord>::fail(std::move(key_result.status));
            
            Result<ArenaEntry> value_result = make_entry(value);
            if (!value_result.is_ok())
                return Result<InternalRecord>::fail(std::move(value_result.status));

            return Result<InternalRecord>::ok(
                    InternalRecord(
                    key_result.value,
                    value_result.value,
                    type,
                    sequence
                )
            );
        }

        void expect_valid()
        {
            EXPECT_TRUE(tree.validate());
            EXPECT_TRUE(RBTree::expect_parent_links_valid(
                tree.root_getter(),
                nullptr
            ));
        }
    };

    TEST_F(RBTreeTest, EmptyTreeHasNoRecords)
    {
        expect_valid();
        EXPECT_EQ(tree.root_getter(), nullptr);
        EXPECT_EQ(tree.approximate_memory_usage(), 0u);

        std::vector<InternalRecord> records;
        tree.dump_inorder(records);
        EXPECT_TRUE(records.empty());

        const auto missing_key = make_entry("missing");
        EXPECT_TRUE(missing_key.is_ok());

        const auto result = tree.find_latest_by_key(missing_key.value);

        EXPECT_FALSE(result.is_ok());
        EXPECT_EQ(status_code_of(result.status), StatusCode::NotFound);
    }

    TEST_F(RBTreeTest, InsertAndFindSingleRecord)
    {
        const auto record = make_record("key", "value", Type::Put, 7);

        EXPECT_TRUE(record.is_ok());

        const Status status = tree.insert(record.value);
        ASSERT_TRUE(status.is_ok());
        expect_valid();

        const auto result = tree.find_latest_by_key(record.value.key_entry);
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value, record);
    }

    TEST_F(RBTreeTest, SameKeyIsOrderedByDescendingSequence)
    {
        const auto old_record = make_record("key", "old", Type::Put, 10);
        const auto newest_record = make_record("key", "newest", Type::Put, 30);
        const auto middle_record = make_record("key", "middle", Type::Put, 20);

        ASSERT_TRUE(old_record.is_ok());
        ASSERT_TRUE(newest_record.is_ok());
        ASSERT_TRUE(middle_record.is_ok());

        ASSERT_TRUE(tree.insert(old_record.value).is_ok());
        ASSERT_TRUE(tree.insert(newest_record.value).is_ok());
        ASSERT_TRUE(tree.insert(middle_record.value).is_ok());
        expect_valid();

        const auto result = tree.find_latest_by_key(old_record.value.key_entry);
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value, newest_record);

        std::vector<InternalRecord> records;
        tree.dump_inorder(records);

        ASSERT_EQ(records.size(), 3u);
        EXPECT_EQ(records[0], newest_record);
        EXPECT_EQ(records[1], middle_record);
        EXPECT_EQ(records[2], old_record);
    }

    TEST_F(RBTreeTest, NewestTombstoneIsReturned)
    {
        const auto put = make_record("key", "value", Type::Put, 4);
        const auto tombstone = make_record("key", "", Type::Tombstone, 5);

        ASSERT_TRUE(put.is_ok());
        ASSERT_TRUE(tombstone.is_ok());

        ASSERT_TRUE(tree.insert(put.value).is_ok());
        ASSERT_TRUE(tree.insert(tombstone.value).is_ok());
        expect_valid();

        const auto result = tree.find_latest_by_key(put.value.key_entry);
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value.type, Type::Tombstone);
        EXPECT_EQ(result.value.seq_num, 5u);
    }

    TEST_F(RBTreeTest, DuplicateIdentityIsKeyAndSequence)
    {
        const auto original = make_record("key", "first", Type::Put, 42);
        const auto different_value = make_record("key", "second", Type::Put, 42);
        const auto different_type = make_record("key", "", Type::Tombstone, 42);

        ASSERT_TRUE(original.is_ok());
        ASSERT_TRUE(different_value.is_ok());
        ASSERT_TRUE(different_type.is_ok());

        ASSERT_TRUE(tree.insert(original.value).is_ok());
        const std::size_t usage_after_first_insert = tree.approximate_memory_usage();

        const Status value_duplicate = tree.insert(different_value.value);
        EXPECT_FALSE(value_duplicate.is_ok());
        EXPECT_EQ(status_code_of(value_duplicate), StatusCode::Duplicate);

        const Status type_duplicate = tree.insert(different_type.value);
        EXPECT_FALSE(type_duplicate.is_ok());
        EXPECT_EQ(status_code_of(type_duplicate), StatusCode::Duplicate);

        EXPECT_EQ(tree.approximate_memory_usage(), usage_after_first_insert);
        expect_valid();

        const auto result = tree.find_latest_by_key(original.value.key_entry);
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value, original);
    }

    TEST_F(RBTreeTest, SameSequenceIsAllowedForDifferentKeys)
    {
        const auto first = make_record("a", "first", Type::Put, 9);
        const auto second = make_record("b", "second", Type::Put, 9);

        EXPECT_TRUE(first.is_ok());
        EXPECT_TRUE(second.is_ok());

        EXPECT_TRUE(tree.insert(first.value).is_ok());
        EXPECT_TRUE(tree.insert(second.value).is_ok());
        expect_valid();
    }

    TEST_F(RBTreeTest, DumpUsesKeyAscendingThenSequenceDescendingOrder)
    {

        const std::array<Result<InternalRecord>, 6> input = {
            make_record("c", "c1", Type::Put, 1),
            make_record("a", "a2", Type::Put, 2),
            make_record("b", "b1", Type::Put, 1),
            make_record("a", "a5", Type::Put, 5),
            make_record("c", "c9", Type::Tombstone, 9),
            make_record("b", "b7", Type::Put, 7)
        };

        for (const auto& record : input) {
            ASSERT_TRUE(record.is_ok());
            ASSERT_TRUE(tree.insert(record.value).is_ok());
        }

        expect_valid();

        std::vector<InternalRecord> records;
        tree.dump_inorder(records);

        const std::array<const InternalRecord*, 6> expected = {
            & (input[3].value), // a, seq 5
            & (input[1].value), // a, seq 2
            & (input[5].value), // b, seq 7
            & (input[2].value), // b, seq 1
            & (input[4].value), // c, seq 9
            & (input[0].value)  // c, seq 1
        };

        ASSERT_EQ(records.size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i)
            EXPECT_EQ(records[i], *expected[i]);
    }

    TEST(RBTreeNodeTest, EqualityMatchesTreeIdentity)
    {
        Arena arena;
        const auto key = ArenaEntry::make_entry(arena, "key");
        const auto first_value = ArenaEntry::make_entry(arena, "first");
        const auto second_value = ArenaEntry::make_entry(arena, "second");

        ASSERT_TRUE(key.is_ok());
        ASSERT_TRUE(first_value.is_ok());
        ASSERT_TRUE(second_value.is_ok());

        const RBTree::Node first(key.value, first_value.value, Type::Put, 11);
        const RBTree::Node same_identity(key.value, second_value.value, Type::Tombstone, 11);

        EXPECT_FALSE(first < same_identity);
        EXPECT_FALSE(same_identity < first);
        EXPECT_EQ(first, same_identity);
    }

    TEST(RBTreeBalancingTest, HandlesAllFourThreeNodeRotationPatterns)
    {
        struct RotationCase
        {
            std::array<std::string, 3> insertion_order;
            std::string expected_root;
            std::string expected_left;
            std::string expected_right;
        };

        const std::array<RotationCase, 4> cases = { {
            {{{"c", "b", "a"}}, "b", "a", "c"}, // left-left
            {{{"a", "b", "c"}}, "b", "a", "c"}, // right-right
            {{{"c", "a", "b"}}, "b", "a", "c"}, // left-right
            {{{"a", "c", "b"}}, "b", "a", "c"}  // right-left
        } };

        for (const auto& test_case : cases)
        {
            SCOPED_TRACE(test_case.insertion_order[0] + test_case.insertion_order[1] + test_case.insertion_order[2]);

            Arena arena;
            RBTree tree;
            std::uint64_t sequence = 1;

            for (const auto& key : test_case.insertion_order)
            {
                Result<ArenaEntry> key_entr = ArenaEntry::make_entry(arena, key);
                ASSERT_TRUE(key_entr.is_ok());
                Result<ArenaEntry> value = ArenaEntry::make_entry(arena, "value");
                ASSERT_TRUE(value.is_ok());
                const InternalRecord record(
                    key_entr.value,
                    value.value,
                    Type::Put,
                    sequence++
                );
                ASSERT_TRUE(tree.insert(record).is_ok());
            }

            ASSERT_TRUE(tree.validate());
            ASSERT_TRUE(RBTree::expect_parent_links_valid(tree.root_getter(), nullptr));

            const RBTree::Node* root = tree.root_getter();
            ASSERT_NE(root, nullptr);
            ASSERT_NE(root->left, nullptr);
            ASSERT_NE(root->right, nullptr);

            EXPECT_EQ(entry_bytes(root->key_entry), test_case.expected_root);
            EXPECT_EQ(entry_bytes(root->left->key_entry), test_case.expected_left);
            EXPECT_EQ(entry_bytes(root->right->key_entry), test_case.expected_right);
            EXPECT_EQ(root->color, RBTree::Node::Color::Black);
            EXPECT_EQ(root->left->color, RBTree::Node::Color::Red);
            EXPECT_EQ(root->right->color, RBTree::Node::Color::Red);
        }
    }

    TEST(RBTreeBalancingTest, RemainsValidForActuallySortedInsertionOrders)
    {
        constexpr int record_count = 1000;

        for (const bool descending : { false, true })
        {
            SCOPED_TRACE(descending ? "descending" : "ascending");

            Arena arena;
            RBTree tree;

            for (int step = 0; step < record_count; ++step)
            {
                const int value = descending
                    ? record_count - 1 - step
                    : step;

                const std::string key = ordered_key(value);

                Result<ArenaEntry> key_entr = ArenaEntry::make_entry(arena, key);
                ASSERT_TRUE(key_entr.is_ok());
                Result<ArenaEntry> value_entr = ArenaEntry::make_entry(arena, "value-" + key);
                ASSERT_TRUE(value_entr.is_ok());

                const InternalRecord record(
                    key_entr.value, 
                    value_entr.value,
                    Type::Put,
                    static_cast<std::uint64_t>(step + 1)
                );

                ASSERT_TRUE(tree.insert(record).is_ok());

                if ((step % 32) == 0)
                {
                    ASSERT_TRUE(tree.validate());
                    ASSERT_TRUE(RBTree::expect_parent_links_valid(
                        tree.root_getter(),
                        nullptr
                    ));
                }
            }

            EXPECT_TRUE(tree.validate());
            EXPECT_TRUE(RBTree::expect_parent_links_valid(tree.root_getter(), nullptr));
        }
    }

    TEST_F(RBTreeTest, SupportsEmptyAndBinaryKeysAndValues)
    {
        const std::string empty;
        const std::string embedded_null("a\0b", 3);
        const std::string leading_null("\0abc", 4);
        const std::string high_byte("\xFF\0", 2);
        const std::string binary_value("v\0x", 3);

        const std::array<Result<InternalRecord>, 4> records = {
            make_record(empty, empty, Type::Put, 1),
            make_record(embedded_null, binary_value, Type::Put, 2),
            make_record(leading_null, empty, Type::Put, 3),
            make_record(high_byte, binary_value, Type::Put, 4)
        };

        for (const auto& record : records) {
            ASSERT_TRUE(record.is_ok());
            ASSERT_TRUE(tree.insert(record.value).is_ok());
        }
        expect_valid();

        for (const auto& expected : records)
        {
            const auto result = tree.find_latest_by_key(expected.value.key_entry);
            ASSERT_TRUE(result.is_ok());
            EXPECT_EQ(result.value, expected);
        }
    }

    TEST_F(RBTreeTest, ArenaGrowthDoesNotInvalidateStoredEntries)
    {
        const auto stable = make_record("stable-key", "stable-value", Type::Put, 1);
        ASSERT_TRUE(stable.is_ok());
        ASSERT_TRUE(tree.insert(stable.value).is_ok());

        for (int i = 0; i < 10000; ++i)
            (void)make_entry("noise-" + std::to_string(i));

        const auto result = tree.find_latest_by_key(stable.value.key_entry);
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(entry_bytes(result.value.key_entry), "stable-key");
        EXPECT_EQ(entry_bytes(result.value.value_entry), "stable-value");
        expect_valid();
    }

    TEST_F(RBTreeTest, SupportsLargeArenaEntries)
    {
        const std::string large_key(32 * 1024, 'k');
        const std::string large_value(64 * 1024, 'v');
        const auto record = make_record(large_key, large_value, Type::Put, 1);

        ASSERT_TRUE(record.is_ok());

        ASSERT_TRUE(tree.insert(record.value).is_ok());
        expect_valid();

        const auto result = tree.find_latest_by_key(record.value.key_entry);
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(entry_bytes(result.value.key_entry), large_key);
        EXPECT_EQ(entry_bytes(result.value.value_entry), large_value);
    }

    TEST(RBTreeDifferentialTest, MatchesReferenceModelUnderDeterministicRandomWorkload)
    {
        struct ExpectedRecord
        {
            std::string value;
            Type type;
        };

        constexpr int operation_count = 5000;
        constexpr int key_count = 128;
        constexpr int sequence_count = 256;

        std::mt19937_64 rng(0x5EED1234ULL);
        std::uniform_int_distribution<int> key_distribution(0, key_count - 1);
        std::uniform_int_distribution<int> sequence_distribution(0, sequence_count - 1);
        std::bernoulli_distribution tombstone_distribution(0.2);

        Arena arena;
        RBTree tree;
        std::map<std::string, std::map<std::uint64_t, ExpectedRecord>> model;

        for (int operation = 0; operation < operation_count; ++operation)
        {
            const std::string key = ordered_key(key_distribution(rng));
            const std::uint64_t sequence = static_cast<std::uint64_t>(
                sequence_distribution(rng)
                );
            const Type type = tombstone_distribution(rng)
                ? Type::Tombstone
                : Type::Put;
            const std::string value = type == Type::Tombstone
                ? std::string{}
            : "value-" + std::to_string(operation);

            const auto [_, inserted] = model[key].emplace(
                sequence,
                ExpectedRecord{ value, type }
            );

            Result<ArenaEntry> key_entry = ArenaEntry::make_entry(arena, key);
            Result<ArenaEntry> value_entry = ArenaEntry::make_entry(arena, value);

            ASSERT_TRUE(key_entry.is_ok());
            ASSERT_TRUE(value_entry.is_ok());

            const InternalRecord record(
                key_entry.value, 
                value_entry.value,
                type,
                sequence
            );

            const Status status = tree.insert(record);
            EXPECT_EQ(status.is_ok(), inserted);

            if (!inserted)
                EXPECT_EQ(status_code_of(status), StatusCode::Duplicate);

            if ((operation % 64) == 0)
            {
                ASSERT_TRUE(tree.validate());
                ASSERT_TRUE(RBTree::expect_parent_links_valid(
                    tree.root_getter(),
                    nullptr
                ));
            }
        }

        ASSERT_TRUE(tree.validate());
        ASSERT_TRUE(RBTree::expect_parent_links_valid(tree.root_getter(), nullptr));

        for (const auto& [key, versions] : model)
        {
            //Result<ArenaEntry> key_entry = ArenaEntry::make_entry(arena, key);

            //ASSERT_TRUE(key_entry.is_ok());

            Result<ArenaEntry> search_entry =
                ArenaEntry::make_entry(arena, key);

            ASSERT_TRUE(search_entry.is_ok());

            const auto result = tree.find_latest_by_key(
                search_entry.value
            );
            ASSERT_TRUE(result.is_ok());

            const auto& [expected_sequence, expected] = *versions.rbegin();
            EXPECT_EQ(result.value.seq_num, expected_sequence);
            EXPECT_EQ(result.value.type, expected.type);
            EXPECT_EQ(entry_bytes(result.value.value_entry), expected.value);
        }

        std::vector<InternalRecord> actual;
        tree.dump_inorder(actual);

        std::size_t expected_record_count = 0;
        for (const auto& [_, versions] : model)
            expected_record_count += versions.size();

        ASSERT_EQ(actual.size(), expected_record_count);

        std::size_t index = 0;
        for (const auto& [key, versions] : model)
        {
            for (auto version = versions.rbegin(); version != versions.rend(); ++version)
            {
                const auto& [sequence, expected] = *version;
                ASSERT_LT(index, actual.size());

                EXPECT_EQ(entry_bytes(actual[index].key_entry), key);
                EXPECT_EQ(actual[index].seq_num, sequence);
                EXPECT_EQ(actual[index].type, expected.type);
                EXPECT_EQ(entry_bytes(actual[index].value_entry), expected.value);
                ++index;
            }
        }

        EXPECT_EQ(index, actual.size());

        Result<ArenaEntry> search_entry = ArenaEntry::make_entry(arena, "definitely-missing");

        ASSERT_TRUE(search_entry.is_ok());

        const auto missing = tree.find_latest_by_key(
            search_entry.value
        );
        EXPECT_FALSE(missing.is_ok());
        EXPECT_EQ(status_code_of(missing.status), StatusCode::NotFound);
    }

} // namespace