#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "file.h"
#include "table_meta.h"

namespace {

    class TempFilePath {
    public:
        TempFilePath()
        {
            static std::atomic<std::uint64_t> counter{ 0 };

            const auto now = std::chrono::high_resolution_clock::now()
                .time_since_epoch()
                .count();

            path_ = std::filesystem::temp_directory_path() /
                ("kvdb_table_meta_test_" +
                    std::to_string(now) + "_" +
                    std::to_string(counter.fetch_add(1)) + ".bin");

            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }

        ~TempFilePath()
        {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }

        const std::filesystem::path& path() const noexcept
        {
            return path_;
        }

    private:
        std::filesystem::path path_;
    };

    TableMeta make_valid_meta()
    {
        // Static storage keeps these shallow ArenaEntry pointers valid throughout
        // each test without coupling construction to an Arena.
        static std::string smallest = "alpha";
        static std::string largest = "omega";

        TableMeta meta{};
        meta.table_id = 42;
        meta.level = 3;
        meta.path = std::filesystem::path("sst") / "000042.sst";

        meta.min_seq = 100;
        meta.max_seq = 900;

        meta.file_size = 32 * 1024;
        meta.record_count = 300;
        meta.tombstone_count = 17;
        meta.data_block_count = 9;
        meta.data_bytes = 24 * 1024;

        meta.smallest_key.data = smallest.data();
        meta.smallest_key.size = static_cast<std::uint32_t>(smallest.size());
        meta.largest_key.data = largest.data();
        meta.largest_key.size = static_cast<std::uint32_t>(largest.size());

        return meta;
    }

    std::string entry_bytes(const ArenaEntry& entry)
    {
        if (entry.size == 0) {
            return {};
        }

        return std::string(
            static_cast<const char*>(entry.data),
            static_cast<std::size_t>(entry.size)
        );
    }

    void expect_same_meta(const TableMeta& lhs, const TableMeta& rhs)
    {
        EXPECT_EQ(lhs.table_id, rhs.table_id);
        EXPECT_EQ(lhs.level, rhs.level);
        EXPECT_EQ(lhs.path, rhs.path);

        EXPECT_EQ(lhs.min_seq, rhs.min_seq);
        EXPECT_EQ(lhs.max_seq, rhs.max_seq);

        EXPECT_EQ(lhs.file_size, rhs.file_size);
        EXPECT_EQ(lhs.record_count, rhs.record_count);
        EXPECT_EQ(lhs.tombstone_count, rhs.tombstone_count);
        EXPECT_EQ(lhs.data_block_count, rhs.data_block_count);
        EXPECT_EQ(lhs.data_bytes, rhs.data_bytes);

        EXPECT_EQ(entry_bytes(lhs.smallest_key), entry_bytes(rhs.smallest_key));
        EXPECT_EQ(entry_bytes(lhs.largest_key), entry_bytes(rhs.largest_key));
    }

} // namespace

TEST(TableMetaTest, RoundTripPreservesAllFieldsSizeAndCrc)
{
    TempFilePath temp;
    const TableMeta expected = make_valid_meta();

    ASSERT_TRUE(expected.validate().is_ok());

    const auto expected_disk_size = expected.disk_size();
    ASSERT_TRUE(expected_disk_size.is_ok());

    std::uint32_t expected_crc = 0;
    ASSERT_TRUE(expected.calculate_crc(expected_crc, true).is_ok());

    auto writable_result = open_writable_file(temp.path());
    ASSERT_TRUE(writable_result.is_ok());
    ASSERT_NE(writable_result.value, nullptr);

    auto writable = std::move(writable_result.value);
    std::uint64_t write_offset = 0;

    ASSERT_TRUE(expected.write(*writable, write_offset).is_ok());
    EXPECT_EQ(write_offset, expected_disk_size.value);

    ASSERT_TRUE(writable->sync().is_ok());
    ASSERT_TRUE(writable->close().is_ok());

    EXPECT_EQ(
        std::filesystem::file_size(temp.path()),
        static_cast<std::uintmax_t>(expected_disk_size.value)
    );

    auto readable_result = open_readable_file(temp.path());
    ASSERT_TRUE(readable_result.is_ok());
    ASSERT_NE(readable_result.value, nullptr);

    auto readable = std::move(readable_result.value);
    Arena load_arena;
    std::uint64_t read_offset = 0;

    auto loaded_result = TableMeta::load(*readable, read_offset, load_arena);
    ASSERT_TRUE(loaded_result.is_ok());
    EXPECT_EQ(read_offset, expected_disk_size.value);

    const TableMeta& loaded = loaded_result.value;
    expect_same_meta(expected, loaded);

    std::uint32_t loaded_crc = 0;
    ASSERT_TRUE(loaded.calculate_crc(loaded_crc, true).is_ok());
    EXPECT_EQ(loaded_crc, expected_crc);

    ASSERT_TRUE(readable->close().is_ok());
}

TEST(TableMetaTest, RejectsNullKeyPointerBeforeWritingAnything)
{
    TempFilePath temp;
    TableMeta meta = make_valid_meta();

    meta.smallest_key.data = nullptr;
    meta.smallest_key.size = 5;

    auto writable_result = open_writable_file(temp.path());
    ASSERT_TRUE(writable_result.is_ok());
    ASSERT_NE(writable_result.value, nullptr);

    auto writable = std::move(writable_result.value);
    std::uint64_t offset = 0;

    const Status status = meta.write(*writable, offset);
    EXPECT_FALSE(status.is_ok());
    EXPECT_EQ(offset, 0u);

    const auto position = writable->current_position();
    ASSERT_TRUE(position.is_ok());
    EXPECT_EQ(position.value, 0u);

    ASSERT_TRUE(writable->close().is_ok());
    EXPECT_EQ(std::filesystem::file_size(temp.path()), 0u);
}

TEST(TableMetaTest, RejectsOversizedVariablePayloadBeforeWritingAnything)
{
    TempFilePath temp;
    TableMeta meta = make_valid_meta();

    std::byte dummy{};
    meta.smallest_key.data = &dummy;
    meta.smallest_key.size = static_cast<std::uint32_t>(MAX_TABLE_META_VARIABLE_BYTES);

    EXPECT_FALSE(meta.validate().is_ok());
    EXPECT_FALSE(meta.disk_size().is_ok());

    auto writable_result = open_writable_file(temp.path());
    ASSERT_TRUE(writable_result.is_ok());
    ASSERT_NE(writable_result.value, nullptr);

    auto writable = std::move(writable_result.value);
    std::uint64_t offset = 0;

    EXPECT_FALSE(meta.write(*writable, offset).is_ok());
    EXPECT_EQ(offset, 0u);

    const auto position = writable->current_position();
    ASSERT_TRUE(position.is_ok());
    EXPECT_EQ(position.value, 0u);

    ASSERT_TRUE(writable->close().is_ok());
}

TEST(TableMetaTest, RejectsBrokenSemanticInvariants)
{
    {
        TableMeta meta = make_valid_meta();
        meta.min_seq = meta.max_seq + 1;
        EXPECT_FALSE(meta.validate().is_ok());
    }

    {
        TableMeta meta = make_valid_meta();
        meta.tombstone_count = meta.record_count + 1;
        EXPECT_FALSE(meta.validate().is_ok());
    }

    {
        TableMeta meta = make_valid_meta();
        meta.data_bytes = meta.file_size + 1;
        EXPECT_FALSE(meta.validate().is_ok());
    }

    {
        static std::string z = "z";
        static std::string a = "a";

        TableMeta meta = make_valid_meta();
        meta.smallest_key.data = z.data();
        meta.smallest_key.size = 1;
        meta.largest_key.data = a.data();
        meta.largest_key.size = 1;

        EXPECT_FALSE(meta.validate().is_ok());
    }
}

TEST(TableMetaTest, LoadOfTruncatedRecordDoesNotAdvanceOffsetOrLeakArena)
{
    TempFilePath temp;
    const TableMeta meta = make_valid_meta();

    const auto disk_size = meta.disk_size();
    ASSERT_TRUE(disk_size.is_ok());
    ASSERT_GT(disk_size.value, 0u);

    auto writable_result = open_writable_file(temp.path());
    ASSERT_TRUE(writable_result.is_ok());
    ASSERT_NE(writable_result.value, nullptr);

    auto writable = std::move(writable_result.value);
    std::uint64_t write_offset = 0;
    ASSERT_TRUE(meta.write(*writable, write_offset).is_ok());
    ASSERT_TRUE(writable->close().is_ok());

    // Remove one byte from the largest key. The loader will have already made
    // Arena allocations by the time it discovers this torn tail.
    std::filesystem::resize_file(temp.path(), disk_size.value - 1);

    auto readable_result = open_readable_file(temp.path());
    ASSERT_TRUE(readable_result.is_ok());
    ASSERT_NE(readable_result.value, nullptr);

    auto readable = std::move(readable_result.value);
    Arena arena;

    const auto used_before = arena.get_used_bytes();
    ASSERT_TRUE(used_before.is_ok());

    std::uint64_t read_offset = 0;
    auto loaded = TableMeta::load(*readable, read_offset, arena);

    EXPECT_FALSE(loaded.is_ok());
    EXPECT_EQ(read_offset, 0u);

    const auto used_after = arena.get_used_bytes();
    ASSERT_TRUE(used_after.is_ok());
    EXPECT_EQ(used_after.value, used_before.value);

    ASSERT_TRUE(readable->close().is_ok());
}

TEST(TableMetaTest, InvalidWriteOffsetIsRejectedWithoutWriting)
{
    TempFilePath temp;
    const TableMeta meta = make_valid_meta();

    auto writable_result = open_writable_file(temp.path());
    ASSERT_TRUE(writable_result.is_ok());
    ASSERT_NE(writable_result.value, nullptr);

    auto writable = std::move(writable_result.value);

    std::uint64_t incorrect_offset = 123;
    EXPECT_FALSE(meta.write(*writable, incorrect_offset).is_ok());
    EXPECT_EQ(incorrect_offset, 123u);

    const auto position = writable->current_position();
    ASSERT_TRUE(position.is_ok());
    EXPECT_EQ(position.value, 0u);

    ASSERT_TRUE(writable->close().is_ok());
}

TEST(TableMetaTest, EmptyKeysRoundTripSafely)
{
    TempFilePath temp;
    TableMeta expected = make_valid_meta();
    expected.record_count = 1;
    expected.tombstone_count = 0;
    expected.smallest_key = ArenaEntry{};
    expected.largest_key = ArenaEntry{};

    ASSERT_TRUE(expected.validate().is_ok());

    auto writable_result = open_writable_file(temp.path());
    ASSERT_TRUE(writable_result.is_ok());
    auto writable = std::move(writable_result.value);

    std::uint64_t write_offset = 0;
    ASSERT_TRUE(expected.write(*writable, write_offset).is_ok());
    ASSERT_TRUE(writable->close().is_ok());

    auto readable_result = open_readable_file(temp.path());
    ASSERT_TRUE(readable_result.is_ok());
    auto readable = std::move(readable_result.value);

    Arena arena;
    std::uint64_t read_offset = 0;
    auto loaded = TableMeta::load(*readable, read_offset, arena);

    ASSERT_TRUE(loaded.is_ok());
    EXPECT_EQ(loaded.value.smallest_key.size, 0u);
    EXPECT_EQ(loaded.value.smallest_key.data, nullptr);
    EXPECT_EQ(loaded.value.largest_key.size, 0u);
    EXPECT_EQ(loaded.value.largest_key.data, nullptr);

    ASSERT_TRUE(readable->close().is_ok());
}