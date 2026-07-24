#include <gtest/gtest.h>

#include "sstable_builder.h"
#include "sstable_loader.h"
#include "sstable_manager.h"
#include "sstable_writer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
    class TemporaryDirectory
    {
    public:
        TemporaryDirectory()
        {
            static std::atomic<std::uint64_t> next_id{ 0 };
            const auto timestamp = std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();

            root_ = std::filesystem::temp_directory_path() /
                ("kvdb_sstable_pipeline_test_" +
                    std::to_string(timestamp) + "_" +
                    std::to_string(next_id.fetch_add(1)));

            std::error_code ec;
            std::filesystem::create_directories(root_, ec);
            if (ec) {
                throw std::runtime_error(
                    "Failed to create test directory: " + ec.message()
                );
            }
        }

        TemporaryDirectory(const TemporaryDirectory&) = delete;
        TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

        ~TemporaryDirectory()
        {
            std::error_code ignored;
            std::filesystem::remove_all(root_, ignored);
        }

        [[nodiscard]] const std::filesystem::path& root() const noexcept
        {
            return root_;
        }

        [[nodiscard]] std::filesystem::path path(
            const std::filesystem::path& child
        ) const
        {
            return root_ / child;
        }

    private:
        std::filesystem::path root_;
    };

    struct OwnedInternalRecord
    {
        std::string key_storage;
        std::string value_storage;
        InternalRecord record{};

        OwnedInternalRecord(
            std::string key,
            std::string value,
            ::Type type,
            std::uint64_t seq_num
        )
            : key_storage(std::move(key)),
            value_storage(std::move(value))
        {
            record.type = type;
            record.seq_num = seq_num;

            record.key_entry.data = key_storage.empty()
                ? nullptr
                : key_storage.data();
            record.key_entry.size = key_storage.size();

            record.value_entry.data = value_storage.empty()
                ? nullptr
                : value_storage.data();
            record.value_entry.size = value_storage.size();
        }

        OwnedInternalRecord(const OwnedInternalRecord&) = delete;
        OwnedInternalRecord& operator=(const OwnedInternalRecord&) = delete;
        OwnedInternalRecord(OwnedInternalRecord&&) = delete;
        OwnedInternalRecord& operator=(OwnedInternalRecord&&) = delete;
    };

    [[nodiscard]] ArenaEntry make_key(std::string& storage)
    {
        ArenaEntry result{};
        result.data = storage.empty() ? nullptr : storage.data();
        result.size = storage.size();
        return result;
    }

    [[nodiscard]] std::string_view entry_view(const ArenaEntry& entry)
    {
        if (entry.size == 0) {
            return {};
        }
        return std::string_view(
            static_cast<const char*>(entry.data),
            entry.size
        );
    }
}

TEST(SSTableBuilderTest, EmptyInputReturnsNoTable)
{
    TemporaryDirectory directory;
    const std::vector<InternalRecord> records;

    auto result = SSTableBuilder::build(
        1,
        records,
        directory.path("table.tmp"),
        directory.path("table.sst")
    );

    ASSERT_TRUE(result.is_ok()) << result.status.message;
    EXPECT_FALSE(result.value.has_value());
}

TEST(SSTableBuilderTest, RejectsUnsortedInput)
{
    TemporaryDirectory directory;
    OwnedInternalRecord beta("beta", "2", ::Type::Put, 1);
    OwnedInternalRecord alpha("alpha", "1", ::Type::Put, 1);

    const std::vector<InternalRecord> records{
        beta.record,
        alpha.record
    };

    auto result = SSTableBuilder::build(
        2,
        records,
        directory.path("table.tmp"),
        directory.path("table.sst")
    );

    ASSERT_FALSE(result.is_ok());
    EXPECT_EQ(result.status.code, StatusCode::InvalidArgument);
    EXPECT_FALSE(std::filesystem::exists(directory.path("table.sst")));
}

TEST(SSTableBuilderTest, RejectsAscendingSequenceForSameKey)
{
    TemporaryDirectory directory;
    OwnedInternalRecord older("alpha", "old", ::Type::Put, 4);
    OwnedInternalRecord newer("alpha", "new", ::Type::Put, 9);

    const std::vector<InternalRecord> records{
        older.record,
        newer.record
    };

    auto result = SSTableBuilder::build(
        3,
        records,
        directory.path("table.tmp"),
        directory.path("table.sst")
    );

    ASSERT_FALSE(result.is_ok());
    EXPECT_EQ(result.status.code, StatusCode::InvalidArgument);
}

TEST(SSTablePipelineTest, BuildWriteLoadAndGetRoundTrips)
{
    TemporaryDirectory directory;
    const auto temporary_path = directory.path("table.tmp");
    const auto final_path = directory.path("table.sst");

    OwnedInternalRecord alpha_new("alpha", "new", ::Type::Put, 9);
    OwnedInternalRecord alpha_old("alpha", "old", ::Type::Put, 4);
    OwnedInternalRecord omega("omega", "", ::Type::Tombstone, 3);

    const std::vector<InternalRecord> records{
        alpha_new.record,
        alpha_old.record,
        omega.record
    };

    auto built = SSTableBuilder::build(
        10,
        records,
        temporary_path,
        final_path
    );
    ASSERT_TRUE(built.is_ok()) << built.status.message;
    ASSERT_TRUE(built.value.has_value());

    SSTable table = std::move(*built.value);
    ASSERT_TRUE(SSTableWriter::write(table).is_ok());
    EXPECT_TRUE(std::filesystem::exists(final_path));
    EXPECT_FALSE(std::filesystem::exists(temporary_path));
    EXPECT_EQ(table.get_path(), final_path);
    EXPECT_EQ(table.get_final_path(), final_path);

    Arena load_arena;
    auto loaded_result = SSTableLoader::load(final_path, load_arena);
    ASSERT_TRUE(loaded_result.is_ok()) << loaded_result.status.message;
    SSTable loaded = std::move(loaded_result.value);
    EXPECT_EQ(loaded.get_path(), final_path);
    EXPECT_EQ(loaded.get_final_path(), final_path);

    std::string alpha_storage = "alpha";
    ArenaEntry alpha_key = make_key(alpha_storage);
    Arena result_arena;

    auto alpha = loaded.get(alpha_key, result_arena);
    ASSERT_TRUE(alpha.is_ok()) << alpha.status.message;
    ASSERT_TRUE(alpha.value.has_value());
    EXPECT_EQ(alpha.value->type, ::Type::Put);
    EXPECT_EQ(alpha.value->seq_num, 9u);
    EXPECT_EQ(entry_view(alpha.value->value_entry), "new");

    std::string omega_storage = "omega";
    ArenaEntry omega_key = make_key(omega_storage);
    auto tombstone = loaded.get(omega_key, result_arena);
    ASSERT_TRUE(tombstone.is_ok()) << tombstone.status.message;
    ASSERT_TRUE(tombstone.value.has_value());
    EXPECT_EQ(tombstone.value->type, ::Type::Tombstone);
    EXPECT_EQ(tombstone.value->seq_num, 3u);
}

TEST(SSTableWriterTest, PublishedTableCannotBeWrittenTwice)
{
    TemporaryDirectory directory;
    OwnedInternalRecord record("alpha", "value", ::Type::Put, 1);
    const std::vector<InternalRecord> records{ record.record };

    auto built = SSTableBuilder::build(
        11,
        records,
        directory.path("table.tmp"),
        directory.path("table.sst")
    );
    ASSERT_TRUE(built.is_ok()) << built.status.message;
    ASSERT_TRUE(built.value.has_value());

    SSTable table = std::move(*built.value);
    ASSERT_TRUE(SSTableWriter::write(table).is_ok());

    const Status second = SSTableWriter::write(table);
    EXPECT_FALSE(second.is_ok());
    EXPECT_EQ(second.code, StatusCode::InvalidState);
}

TEST(SSTableLoaderTest, TruncatedPublishedTableIsRejected)
{
    TemporaryDirectory directory;
    const auto final_path = directory.path("table.sst");
    OwnedInternalRecord record("alpha", "value", ::Type::Put, 1);
    const std::vector<InternalRecord> records{ record.record };

    auto built = SSTableBuilder::build(
        12,
        records,
        directory.path("table.tmp"),
        final_path
    );
    ASSERT_TRUE(built.is_ok()) << built.status.message;
    ASSERT_TRUE(built.value.has_value());

    SSTable table = std::move(*built.value);
    ASSERT_TRUE(SSTableWriter::write(table).is_ok());

    const auto original_size = std::filesystem::file_size(final_path);
    ASSERT_GT(original_size, 1u);
    std::filesystem::resize_file(final_path, original_size / 2);

    Arena arena;
    auto loaded = SSTableLoader::load(final_path, arena);
    EXPECT_FALSE(loaded.is_ok());
}

TEST(SSTableManagerTest, OpenFromMetaDoesNotMoveOrModifyMetaPath)
{
    TemporaryDirectory directory;
    SSTableManager manager(directory.root());

    TableMeta meta{};
    meta.table_id = 77;
    meta.path = directory.path("missing.sst");
    const auto original_path = meta.path;

    Arena arena;
    const auto result = manager.open(meta, arena);

    EXPECT_FALSE(result.is_ok());
    EXPECT_EQ(meta.path, original_path);
}

TEST(SSTableManagerTest, OpenCachesLoadedTableById)
{
    TemporaryDirectory directory;
    const std::uint32_t table_id = 42;
    const auto final_path = directory.path("table-000000042.sst");

    OwnedInternalRecord record("alpha", "value", ::Type::Put, 1);
    const std::vector<InternalRecord> records{ record.record };
    auto built = SSTableBuilder::build(
        table_id,
        records,
        directory.path("table-000000042.sst.tmp"),
        final_path
    );
    ASSERT_TRUE(built.is_ok()) << built.status.message;
    ASSERT_TRUE(built.value.has_value());

    SSTable table = std::move(*built.value);
    ASSERT_TRUE(SSTableWriter::write(table).is_ok());

    SSTableManager manager(directory.root());
    Arena load_arena;

    auto first = manager.open(table_id, load_arena);
    ASSERT_TRUE(first.is_ok()) << first.status.message;
    auto second = manager.get(table_id, load_arena);
    ASSERT_TRUE(second.is_ok()) << second.status.message;

    EXPECT_EQ(first.value.get(), second.value.get());
}

TEST(SSTableStreamingBuilderTest, EmptyFinishProducesNoFileAndNoMeta)
{
    TemporaryDirectory directory;
    SSTableManager manager(directory.root());
    auto builder = manager.create_streaming_builder(7);
    ASSERT_NE(builder, nullptr);
    EXPECT_TRUE(builder->empty());

    Arena arena;
    auto result = builder->finish(0, arena);

    ASSERT_TRUE(result.is_ok()) << result.status.message;
    EXPECT_FALSE(result.value.has_value());
    EXPECT_FALSE(std::filesystem::exists(
        directory.path("table-000000007.sst")
    ));
    EXPECT_FALSE(std::filesystem::exists(
        directory.path("table-000000007.sst.tmp")
    ));
}

TEST(SSTableStreamingBuilderTest, RejectsOutOfOrderRecords)
{
    TemporaryDirectory directory;
    SSTableManager manager(directory.root());
    auto builder = manager.create_streaming_builder(8);
    ASSERT_NE(builder, nullptr);

    OwnedInternalRecord beta("beta", "2", ::Type::Put, 1);
    OwnedInternalRecord alpha("alpha", "1", ::Type::Put, 1);

    ASSERT_TRUE(builder->add(beta.record).is_ok());
    const Status status = builder->add(alpha.record);

    EXPECT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::InvalidArgument);
}