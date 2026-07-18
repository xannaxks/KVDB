#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "arena.h"
#include "mem_table.h"

namespace
{

    class MemTableTest : public ::testing::Test
    {
    protected:
        Arena arena_;
        MemTable table_;

        ArenaEntry make_entry(const std::string& text)
        {
            Result<ArenaEntry> result = ArenaEntry::make_entry(arena_, text);
            EXPECT_TRUE(result.is_ok()) << result.status.message;

            if (!result.is_ok())
                return ArenaEntry{};

            return result.value;
        }

        static std::string to_string(const ArenaEntry& entry)
        {
            if (entry.size == 0)
                return {};

            return std::string(
                static_cast<const char*>(entry.data),
                static_cast<std::size_t>(entry.size)
            );
        }

        static void expect_record(
            const Result<InternalRecord>& result,
            const std::string& expected_key,
            const std::string& expected_value,
            Type expected_type,
            std::uint64_t expected_sequence
        )
        {
            ASSERT_TRUE(result.is_ok()) << result.status.message;
            EXPECT_EQ(to_string(result.value.key_entry), expected_key);
            EXPECT_EQ(to_string(result.value.value_entry), expected_value);
            EXPECT_EQ(result.value.type, expected_type);
            EXPECT_EQ(result.value.seq_num, expected_sequence);
        }
    };

    TEST_F(MemTableTest, EmptyTableReportsNotFoundAndZeroUsage)
    {
        const ArenaEntry key = make_entry("missing");

        const Result<InternalRecord> result = table_.get(key);

        ASSERT_FALSE(result.is_ok());
        EXPECT_EQ(result.status.code, StatusCode::NotFound);
        EXPECT_FALSE(table_.has_immutable());
        EXPECT_EQ(table_.immutable_count(), 0u);
        EXPECT_EQ(table_.mutable_memory_usage(), 0u);
        EXPECT_EQ(table_.approximate_memory_usage(), 0u);

        const Result<MemTable::ImmutableSnapshot> snapshot =
            table_.oldest_immutable();

        ASSERT_FALSE(snapshot.is_ok());
        EXPECT_EQ(snapshot.status.code, StatusCode::NotFound);
    }

    TEST_F(MemTableTest, PutStoresAndReturnsRecord)
    {
        const ArenaEntry key = make_entry("alpha");
        const ArenaEntry value = make_entry("one");

        const Status status = table_.put(key, value, 7);

        ASSERT_TRUE(status.is_ok()) << status.message;
        expect_record(table_.get(key), "alpha", "one", Type::Put, 7);
        EXPECT_GT(table_.mutable_memory_usage(), 0u);
    }

    TEST_F(MemTableTest, HighestSequenceWinsWithinMutableTable)
    {
        const ArenaEntry key = make_entry("key");
        const ArenaEntry value_one = make_entry("one");
        const ArenaEntry value_nine = make_entry("nine");
        const ArenaEntry value_three = make_entry("three");

        ASSERT_TRUE(table_.put(key, value_one, 1).is_ok());
        ASSERT_TRUE(table_.put(key, value_nine, 9).is_ok());
        ASSERT_TRUE(table_.put(key, value_three, 3).is_ok());

        expect_record(table_.get(key), "key", "nine", Type::Put, 9);
    }

    TEST_F(MemTableTest, RemoveReturnsSuccessfulTombstone)
    {
        const ArenaEntry key = make_entry("deleted-key");
        const ArenaEntry value = make_entry("old-value");

        ASSERT_TRUE(table_.put(key, value, 1).is_ok());
        ASSERT_TRUE(table_.remove(key, 2).is_ok());

        // A tombstone is a successful MemTable lookup. The Engine must not turn it
        // into NotFound and continue searching older SSTables.
        expect_record(
            table_.get(key),
            "deleted-key",
            "",
            Type::Tombstone,
            2
        );
    }

    TEST_F(MemTableTest, DuplicateKeyAndSequenceIsRejectedWithoutReplacement)
    {
        const ArenaEntry key = make_entry("duplicate-key");
        const ArenaEntry first_value = make_entry("first");

        ASSERT_TRUE(table_.put(key, first_value, 11).is_ok());

        const Status duplicate_status = table_.apply(InternalRecord(
            key,
            ArenaEntry{},
            Type::Tombstone,
            11
        ));

        ASSERT_FALSE(duplicate_status.is_ok());
        EXPECT_EQ(duplicate_status.code, StatusCode::Duplicate);
        expect_record(
            table_.get(key),
            "duplicate-key",
            "first",
            Type::Put,
            11
        );
    }

    TEST_F(MemTableTest, FreezingEmptyTableIsNoOpAndDoesNotConsumeGeneration)
    {
        ASSERT_TRUE(table_.freeze_mutable().is_ok());
        ASSERT_TRUE(table_.manual_freeze().is_ok());

        EXPECT_FALSE(table_.has_immutable());
        EXPECT_EQ(table_.immutable_count(), 0u);

        const ArenaEntry key = make_entry("key");
        const ArenaEntry value = make_entry("value");
        ASSERT_TRUE(table_.put(key, value, 1).is_ok());
        ASSERT_TRUE(table_.freeze_mutable().is_ok());

        const Result<MemTable::ImmutableSnapshot> snapshot =
            table_.oldest_immutable();

        ASSERT_TRUE(snapshot.is_ok()) << snapshot.status.message;
        EXPECT_EQ(snapshot.value.generation_id, 1u);
    }

    TEST_F(MemTableTest, FrozenRecordsRemainReadableAlongsideNewMutableRecords)
    {
        const ArenaEntry old_key = make_entry("old-key");
        const ArenaEntry old_value = make_entry("old-value");
        const ArenaEntry new_key = make_entry("new-key");
        const ArenaEntry new_value = make_entry("new-value");

        ASSERT_TRUE(table_.put(old_key, old_value, 1).is_ok());
        ASSERT_TRUE(table_.freeze_mutable().is_ok());
        ASSERT_TRUE(table_.put(new_key, new_value, 2).is_ok());

        EXPECT_TRUE(table_.has_immutable());
        EXPECT_EQ(table_.immutable_count(), 1u);
        expect_record(table_.get(old_key), "old-key", "old-value", Type::Put, 1);
        expect_record(table_.get(new_key), "new-key", "new-value", Type::Put, 2);
    }

    TEST_F(MemTableTest, NewerMutableRecordOverridesOlderImmutableRecord)
    {
        const ArenaEntry key = make_entry("same-key");
        const ArenaEntry old_value = make_entry("old");
        const ArenaEntry new_value = make_entry("new");

        ASSERT_TRUE(table_.put(key, old_value, 10).is_ok());
        ASSERT_TRUE(table_.freeze_mutable().is_ok());
        ASSERT_TRUE(table_.put(key, new_value, 11).is_ok());

        expect_record(table_.get(key), "same-key", "new", Type::Put, 11);
    }

    TEST_F(MemTableTest, HighestSequenceWinsEvenWhenItIsInOlderGeneration)
    {
        const ArenaEntry key = make_entry("recovery-key");
        const ArenaEntry high_value = make_entry("high-sequence");
        const ArenaEntry low_value = make_entry("low-sequence");

        ASSERT_TRUE(table_.put(key, high_value, 100).is_ok());
        ASSERT_TRUE(table_.freeze_mutable().is_ok());

        // This can happen in recovery tests or malformed caller ordering. Lookup is
        // based on sequence number rather than only generation recency.
        ASSERT_TRUE(table_.put(key, low_value, 50).is_ok());

        expect_record(
            table_.get(key),
            "recovery-key",
            "high-sequence",
            Type::Put,
            100
        );
    }

    TEST_F(MemTableTest, NewerTombstoneAcrossGenerationsWins)
    {
        const ArenaEntry key = make_entry("key");
        const ArenaEntry value = make_entry("value");

        ASSERT_TRUE(table_.put(key, value, 1).is_ok());
        ASSERT_TRUE(table_.freeze_mutable().is_ok());
        ASSERT_TRUE(table_.remove(key, 2).is_ok());
        ASSERT_TRUE(table_.freeze_mutable().is_ok());

        expect_record(table_.get(key), "key", "", Type::Tombstone, 2);
    }

    TEST_F(MemTableTest, ImmutableGenerationIdsAreMonotonic)
    {
        const ArenaEntry key_one = make_entry("one");
        const ArenaEntry value_one = make_entry("value-one");
        const ArenaEntry key_two = make_entry("two");
        const ArenaEntry value_two = make_entry("value-two");

        ASSERT_TRUE(table_.put(key_one, value_one, 1).is_ok());
        ASSERT_TRUE(table_.freeze_mutable().is_ok());
        ASSERT_TRUE(table_.put(key_two, value_two, 2).is_ok());
        ASSERT_TRUE(table_.freeze_mutable().is_ok());

        ASSERT_EQ(table_.immutable_count(), 2u);

        Result<MemTable::ImmutableSnapshot> snapshot = table_.oldest_immutable();
        ASSERT_TRUE(snapshot.is_ok());
        EXPECT_EQ(snapshot.value.generation_id, 1u);
        ASSERT_TRUE(table_.retire_oldest_immutable(1));

        snapshot = table_.oldest_immutable();
        ASSERT_TRUE(snapshot.is_ok());
        EXPECT_EQ(snapshot.value.generation_id, 2u);
    }

    TEST_F(MemTableTest, StaleRetirementDoesNotRemoveAnotherGeneration)
    {
        const ArenaEntry key_one = make_entry("one");
        const ArenaEntry value_one = make_entry("value-one");
        const ArenaEntry key_two = make_entry("two");
        const ArenaEntry value_two = make_entry("value-two");

        ASSERT_TRUE(table_.put(key_one, value_one, 1).is_ok());
        ASSERT_TRUE(table_.freeze_mutable().is_ok());
        ASSERT_TRUE(table_.put(key_two, value_two, 2).is_ok());
        ASSERT_TRUE(table_.freeze_mutable().is_ok());

        EXPECT_FALSE(table_.retire_oldest_immutable(999));
        EXPECT_EQ(table_.immutable_count(), 2u);

        Result<MemTable::ImmutableSnapshot> snapshot = table_.oldest_immutable();
        ASSERT_TRUE(snapshot.is_ok());
        EXPECT_EQ(snapshot.value.generation_id, 1u);

        EXPECT_TRUE(table_.retire_oldest_immutable(1));
        EXPECT_FALSE(table_.retire_oldest_immutable(1));
        EXPECT_EQ(table_.immutable_count(), 1u);

        snapshot = table_.oldest_immutable();
        ASSERT_TRUE(snapshot.is_ok());
        EXPECT_EQ(snapshot.value.generation_id, 2u);
    }

    TEST_F(MemTableTest, SnapshotKeepsFrozenTreeAliveAfterQueueRetirement)
    {
        const ArenaEntry key = make_entry("snapshot-key");
        const ArenaEntry value = make_entry("snapshot-value");

        ASSERT_TRUE(table_.put(key, value, 1).is_ok());
        ASSERT_TRUE(table_.freeze_mutable().is_ok());

        Result<MemTable::ImmutableSnapshot> snapshot = table_.oldest_immutable();
        ASSERT_TRUE(snapshot.is_ok());
        ASSERT_NE(snapshot.value.table, nullptr);

        ASSERT_TRUE(table_.retire_oldest_immutable(snapshot.value.generation_id));
        EXPECT_FALSE(table_.has_immutable());

        const Result<InternalRecord> result =
            snapshot.value.table->find_latest_by_key(key);

        ASSERT_TRUE(result.is_ok()) << result.status.message;
        EXPECT_EQ(to_string(result.value.value_entry), "snapshot-value");
        EXPECT_EQ(result.value.seq_num, 1u);
    }

    TEST_F(MemTableTest, RetiringGenerationRemovesItsRecordsFromMemTableReads)
    {
        const ArenaEntry key = make_entry("retired-key");
        const ArenaEntry value = make_entry("retired-value");

        ASSERT_TRUE(table_.put(key, value, 1).is_ok());
        ASSERT_TRUE(table_.freeze_mutable().is_ok());

        const Result<MemTable::ImmutableSnapshot> snapshot =
            table_.oldest_immutable();
        ASSERT_TRUE(snapshot.is_ok());
        ASSERT_TRUE(table_.retire_oldest_immutable(snapshot.value.generation_id));

        const Result<InternalRecord> result = table_.get(key);
        ASSERT_FALSE(result.is_ok());
        EXPECT_EQ(result.status.code, StatusCode::NotFound);
    }

    TEST_F(MemTableTest, DumpOldestImmutableReplacesOutputAndUsesTreeOrdering)
    {
        const ArenaEntry key_a = make_entry("a");
        const ArenaEntry key_b = make_entry("b");
        const ArenaEntry key_c = make_entry("c");
        const ArenaEntry value_a1 = make_entry("a-one");
        const ArenaEntry value_a3 = make_entry("a-three");
        const ArenaEntry value_b = make_entry("b-one");
        const ArenaEntry sentinel_key = make_entry("sentinel");
        const ArenaEntry sentinel_value = make_entry("sentinel-value");

        ASSERT_TRUE(table_.put(key_b, value_b, 1).is_ok());
        ASSERT_TRUE(table_.put(key_a, value_a1, 1).is_ok());
        ASSERT_TRUE(table_.put(key_a, value_a3, 3).is_ok());
        ASSERT_TRUE(table_.remove(key_c, 2).is_ok());
        ASSERT_TRUE(table_.freeze_mutable().is_ok());

        std::vector<InternalRecord> records{
            InternalRecord(sentinel_key, sentinel_value, Type::Put, 999)
        };
        std::uint64_t generation_id = 0;

        const Status status = table_.dump_oldest_immutable(
            records,
            generation_id
        );

        ASSERT_TRUE(status.is_ok()) << status.message;
        EXPECT_EQ(generation_id, 1u);
        ASSERT_EQ(records.size(), 4u); // Existing sentinel was replaced, not appended.

        EXPECT_EQ(to_string(records[0].key_entry), "a");
        EXPECT_EQ(records[0].seq_num, 3u);
        EXPECT_EQ(to_string(records[0].value_entry), "a-three");

        EXPECT_EQ(to_string(records[1].key_entry), "a");
        EXPECT_EQ(records[1].seq_num, 1u);
        EXPECT_EQ(to_string(records[1].value_entry), "a-one");

        EXPECT_EQ(to_string(records[2].key_entry), "b");
        EXPECT_EQ(records[2].seq_num, 1u);

        EXPECT_EQ(to_string(records[3].key_entry), "c");
        EXPECT_EQ(records[3].seq_num, 2u);
        EXPECT_EQ(records[3].type, Type::Tombstone);
    }

    TEST_F(MemTableTest, DumpWithoutImmutablePreservesOutputArguments)
    {
        const ArenaEntry key = make_entry("sentinel");
        const ArenaEntry value = make_entry("value");
        std::vector<InternalRecord> records{
            InternalRecord(key, value, Type::Put, 42)
        };
        std::uint64_t generation_id = 1234;

        const Status status = table_.dump_oldest_immutable(
            records,
            generation_id
        );

        ASSERT_FALSE(status.is_ok());
        EXPECT_EQ(status.code, StatusCode::NotFound);
        ASSERT_EQ(records.size(), 1u);
        EXPECT_EQ(records.front().seq_num, 42u);
        EXPECT_EQ(generation_id, 1234u);
    }

    TEST_F(MemTableTest, MemoryUsageIncludesMutableAndImmutableTables)
    {
        const ArenaEntry key_one = make_entry("one");
        const ArenaEntry value_one = make_entry("value-one");
        const ArenaEntry key_two = make_entry("two");
        const ArenaEntry value_two = make_entry("value-two");

        ASSERT_TRUE(table_.put(key_one, value_one, 1).is_ok());
        const std::size_t first_table_usage = table_.mutable_memory_usage();
        ASSERT_GT(first_table_usage, 0u);
        EXPECT_EQ(table_.approximate_memory_usage(), first_table_usage);

        ASSERT_TRUE(table_.freeze_mutable().is_ok());
        EXPECT_EQ(table_.mutable_memory_usage(), 0u);
        EXPECT_EQ(table_.approximate_memory_usage(), first_table_usage);

        ASSERT_TRUE(table_.put(key_two, value_two, 2).is_ok());
        const std::size_t second_table_usage = table_.mutable_memory_usage();
        EXPECT_GT(second_table_usage, 0u);
        EXPECT_EQ(
            table_.approximate_memory_usage(),
            first_table_usage + second_table_usage
        );

        const Result<MemTable::ImmutableSnapshot> snapshot =
            table_.oldest_immutable();
        ASSERT_TRUE(snapshot.is_ok());
        ASSERT_TRUE(table_.retire_oldest_immutable(snapshot.value.generation_id));

        EXPECT_EQ(table_.approximate_memory_usage(), second_table_usage);
    }

    TEST_F(MemTableTest, ConcurrentReadersAndWriterSmokeTest)
    {
        constexpr std::uint64_t final_sequence = 400;
        constexpr int reader_count = 4;

        const ArenaEntry key = make_entry("shared-key");
        const ArenaEntry value = make_entry("shared-value");

        ASSERT_TRUE(table_.put(key, value, 1).is_ok());

        //std::atomic<bool> start{ false };
        //std::atomic<bool> writer_done{ false };
        //std::atomic<bool> failed{ false };
        std::vector<std::thread> readers;
        readers.reserve(reader_count);

        for (int i = 0; i < reader_count; ++i)
        {
            readers.emplace_back([&]()
                {
                    while (!start.load(std::memory_order_acquire))
                        std::this_thread::yield();

                    while (!writer_done.load(std::memory_order_acquire))
                    {
                        const Result<InternalRecord> result = table_.get(key);
                        if (!result.is_ok() ||
                            result.value.type != Type::Put ||
                            result.value.seq_num == 0 ||
                            result.value.seq_num > final_sequence)
                        {
                            failed.store(true, std::memory_order_release);
                            return;
                        }
                    }
                });
        }

        std::thread writer([&]()
            {
                start.store(true, std::memory_order_release);

                for (std::uint64_t sequence = 2;
                    sequence <= final_sequence;
                    ++sequence)
                {
                    if (!table_.put(key, value, sequence).is_ok())
                    {
                        failed.store(true, std::memory_order_release);
                        break;
                    }

                    if (sequence % 50 == 0 && !table_.freeze_mutable().is_ok())
                    {
                        failed.store(true, std::memory_order_release);
                        break;
                    }
                }

                writer_done.store(true, std::memory_order_release);
            });

        writer.join();
        for (std::thread& reader : readers)
            reader.join();

        EXPECT_FALSE(failed.load(std::memory_order_acquire));
        expect_record(
            table_.get(key),
            "shared-key",
            "shared-value",
            Type::Put,
            final_sequence
        );
    }

} // namespace