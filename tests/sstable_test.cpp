#include <gtest/gtest.h>

#include "sstable.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace
{
    class TemporaryDirectory
    {
    public:
        TemporaryDirectory()
        {
            static std::atomic<std::uint64_t> next_id{ 0 };

            const auto timestamp =
                std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();

            root_ =
                std::filesystem::temp_directory_path() /
                (
                    "kvdb_sstable_test_" +
                    std::to_string(timestamp) +
                    "_" +
                    std::to_string(next_id.fetch_add(1))
                    );

            std::error_code error;
            std::filesystem::create_directories(root_, error);

            if (error)
            {
                throw std::runtime_error(
                    "Failed to create test directory: " +
                    error.message()
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

        [[nodiscard]]
        std::filesystem::path path(
            const std::filesystem::path& child
        ) const
        {
            return root_ / child;
        }

    private:
        std::filesystem::path root_;
    };

    void write_bytes(
        const std::filesystem::path& path,
        std::string_view bytes
    )
    {
        std::ofstream output(
            path,
            std::ios::binary | std::ios::trunc
        );

        ASSERT_TRUE(output.is_open());

        output.write(
            bytes.data(),
            static_cast<std::streamsize>(bytes.size())
        );

        ASSERT_TRUE(output.good());
    }

    [[nodiscard]]
    std::vector<char> read_bytes(
        const std::filesystem::path& path
    )
    {
        std::ifstream input(path, std::ios::binary);

        if (!input.is_open())
            return {};

        return std::vector<char>(
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()
        );
    }

    void corrupt_first_footer_byte(
        const std::filesystem::path& path
    )
    {
        const std::uintmax_t file_size =
            std::filesystem::file_size(path);

        const std::uint64_t footer_size =
            SSTableEntities::FileFooterSection::disk_size();

        ASSERT_GE(file_size, footer_size);

        const std::uint64_t footer_offset =
            static_cast<std::uint64_t>(file_size) -
            footer_size;

        std::fstream file(
            path,
            std::ios::binary |
            std::ios::in |
            std::ios::out
        );

        ASSERT_TRUE(file.is_open());

        file.seekg(
            static_cast<std::streamoff>(footer_offset),
            std::ios::beg
        );

        ASSERT_TRUE(file.good());

        char value = 0;
        file.read(&value, sizeof(value));

        ASSERT_TRUE(file.good());

        value ^= static_cast<char>(0x5A);

        file.seekp(
            static_cast<std::streamoff>(footer_offset),
            std::ios::beg
        );

        ASSERT_TRUE(file.good());

        file.write(&value, sizeof(value));
        file.flush();

        ASSERT_TRUE(file.good());
    }

    void flip_byte_at(
        const std::filesystem::path& path,
        std::uint64_t offset
    )
    {
        std::fstream file(
            path,
            std::ios::binary |
            std::ios::in |
            std::ios::out
        );

        ASSERT_TRUE(file.is_open());
        file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        ASSERT_TRUE(file.good());

        char value = 0;
        file.read(&value, sizeof(value));
        ASSERT_TRUE(file.good());

        value ^= static_cast<char>(0x01);
        file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
        ASSERT_TRUE(file.good());
        file.write(&value, sizeof(value));
        file.flush();
        ASSERT_TRUE(file.good());
    }

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

    [[nodiscard]] std::uint64_t arena_used_bytes(const Arena& arena)
    {
        Result<std::uint64_t> used = arena.get_used_bytes();
        return used.is_ok() ? used.value : 0;
    }
}

/*
 * Default-constructed SSTable has State::Empty.
 * It must not be writable.
 */
TEST(SSTableStateTest, DefaultConstructedTableCannotBeWritten)
{
    SSTable table;

    const Status status = table.write();

    EXPECT_FALSE(status.is_ok());
}

/*
 * The one-path constructor represents an already existing/loaded table.
 *
 * This test is particularly important: write() must reject the operation
 * before calling open_writable_file(), otherwise the existing file could be
 * truncated.
 */
TEST(SSTableStateTest, LoadedTableCannotBeRewrittenOrTruncated)
{
    TemporaryDirectory directory;

    const std::filesystem::path existing_path =
        directory.path("existing.sst");

    constexpr std::string_view original_content =
        "important existing SSTable bytes";

    write_bytes(existing_path, original_content);

    const std::vector<char> bytes_before =
        read_bytes(existing_path);

    SSTable table(existing_path);

    const Status status = table.write();

    EXPECT_FALSE(status.is_ok());
    EXPECT_TRUE(std::filesystem::exists(existing_path));
    EXPECT_EQ(read_bytes(existing_path), bytes_before);
}

/*
 * The temporary and final paths must differ.
 *
 * This must fail before the path is opened, so the existing destination file
 * remains unchanged.
 */
TEST(SSTableStateTest, SameTemporaryAndFinalPathIsRejectedSafely)
{
    TemporaryDirectory directory;

    const std::filesystem::path path =
        directory.path("same-path.sst");

    constexpr std::string_view original_content =
        "do not truncate this file";

    write_bytes(path, original_content);

    const std::vector<char> bytes_before =
        read_bytes(path);

    SSTable table(path, path);

    const Status status = table.write();

    EXPECT_FALSE(status.is_ok());
    EXPECT_EQ(read_bytes(path), bytes_before);
}

/*
 * A builder cannot work without a temporary path.
 */
TEST(SSTableStateTest, EmptyTemporaryPathIsRejected)
{
    TemporaryDirectory directory;

    SSTable table(
        std::filesystem::path{},
        directory.path("final.sst")
    );

    const Status status = table.write();

    EXPECT_FALSE(status.is_ok());
    EXPECT_FALSE(
        std::filesystem::exists(directory.path("final.sst"))
    );
}

/*
 * A builder cannot publish without a final path.
 */
TEST(SSTableStateTest, EmptyFinalPathIsRejected)
{
    TemporaryDirectory directory;

    const std::filesystem::path temporary_path =
        directory.path("table.tmp");

    SSTable table(
        temporary_path,
        std::filesystem::path{}
    );

    const Status status = table.write();

    EXPECT_FALSE(status.is_ok());
    EXPECT_FALSE(std::filesystem::exists(temporary_path));
}

/*
 * A failure before rename must leave the table in State::Building.
 *
 * The first write fails because the temporary directory does not exist.
 * After creating that directory, retrying the same builder should succeed.
 */
TEST(SSTableStateTest, FailedOpenDoesNotConsumeBuilderState)
{
    TemporaryDirectory directory;

    const std::filesystem::path temporary_path =
        directory.path("missing/table.tmp");

    const std::filesystem::path final_path =
        directory.path("table.sst");

    SSTable table(temporary_path, final_path);

    const Status first_status = table.write();

    EXPECT_FALSE(first_status.is_ok());
    EXPECT_FALSE(std::filesystem::exists(final_path));

    std::error_code error;

    std::filesystem::create_directories(
        temporary_path.parent_path(),
        error
    );

    ASSERT_FALSE(error);

    const Status second_status = table.write();

    ASSERT_TRUE(second_status.is_ok());
    EXPECT_TRUE(std::filesystem::exists(final_path));
    EXPECT_FALSE(std::filesystem::exists(temporary_path));
}

/*
 * A successfully written builder transitions to State::Published.
 *
 * A second write must fail before opening/truncating the final file.
 */
TEST(SSTableStateTest, SuccessfulTableCanOnlyBePublishedOnce)
{
    TemporaryDirectory directory;

    const std::filesystem::path temporary_path =
        directory.path("table.tmp");

    const std::filesystem::path final_path =
        directory.path("table.sst");

    SSTable table(temporary_path, final_path);

    const Status first_status = table.write();

    ASSERT_TRUE(first_status.is_ok());
    ASSERT_TRUE(std::filesystem::exists(final_path));
    EXPECT_FALSE(std::filesystem::exists(temporary_path));

    EXPECT_EQ(
        table.get_path().lexically_normal(),
        final_path.lexically_normal()
    );

    EXPECT_EQ(
        table.get_final_path().lexically_normal(),
        final_path.lexically_normal()
    );

    const std::vector<char> bytes_before =
        read_bytes(final_path);

    ASSERT_FALSE(bytes_before.empty());

    const Status second_status = table.write();

    EXPECT_FALSE(second_status.is_ok());
    EXPECT_TRUE(std::filesystem::exists(final_path));
    EXPECT_EQ(read_bytes(final_path), bytes_before);
}

/*
 * append_record() must reject already published tables.
 *
 * The default-constructed record is never inspected because the state check
 * should return before DataSection::add_payload().
 *
 * If InternalRecord is not default constructible, replace InternalRecord{}
 * here with your normal test-record factory.
 */
TEST(SSTableStateTest, PublishedTableRejectsAppend)
{
    if constexpr (!std::is_default_constructible_v<InternalRecord>)
    {
        GTEST_SKIP()
            << "Replace InternalRecord{} with the project's record factory";
    }
    else
    {
        TemporaryDirectory directory;

        SSTable table(
            directory.path("table.tmp"),
            directory.path("table.sst")
        );

        ASSERT_TRUE(table.write().is_ok());

        InternalRecord record{};

        const Status status = table.append_record(record);

        EXPECT_FALSE(status.is_ok());
    }
}

/*
 * append_record() must also reject loaded tables.
 */
TEST(SSTableStateTest, LoadedTableRejectsAppend)
{
    if constexpr (!std::is_default_constructible_v<InternalRecord>)
    {
        GTEST_SKIP()
            << "Replace InternalRecord{} with the project's record factory";
    }
    else
    {
        TemporaryDirectory directory;

        SSTable table(directory.path("existing.sst"));

        InternalRecord record{};

        const Status status = table.append_record(record);

        EXPECT_FALSE(status.is_ok());
    }
}

/*
 * Loading from an empty path must fail before any filesystem operation.
 */
TEST(SSTableLoadTest, EmptyPathIsRejected)
{
    Arena arena;

    auto result = SSTable::load(
        std::filesystem::path{},
        arena
    );

    EXPECT_FALSE(result.is_ok());
}

/*
 * Basic write/load round trip.
 *
 * This verifies that:
 *   - the footer can be found;
 *   - declared section ranges pass validation;
 *   - all section loaders succeed;
 *   - the returned table is State::Loaded and therefore immutable.
 */
TEST(SSTableLoadTest, WrittenTableCanBeLoaded)
{
    TemporaryDirectory directory;

    const std::filesystem::path temporary_path =
        directory.path("table.tmp");

    const std::filesystem::path final_path =
        directory.path("table.sst");

    SSTable builder(temporary_path, final_path);

    ASSERT_TRUE(builder.write().is_ok());
    ASSERT_TRUE(std::filesystem::exists(final_path));

    Arena arena;

    auto load_result = SSTable::load(final_path, arena);

    ASSERT_TRUE(load_result.is_ok());

    SSTable loaded_table =
        std::move(load_result.value);

    EXPECT_EQ(
        loaded_table.get_path().lexically_normal(),
        final_path.lexically_normal()
    );

    /*
     * The table returned by load() must not be writable.
     */
    EXPECT_FALSE(loaded_table.write().is_ok());
}

/*
 * Removing part of the footer must make the file invalid.
 */
TEST(SSTableLoadTest, TruncatedTableIsRejected)
{
    TemporaryDirectory directory;

    const std::filesystem::path temporary_path =
        directory.path("table.tmp");

    const std::filesystem::path final_path =
        directory.path("table.sst");

    SSTable builder(temporary_path, final_path);

    ASSERT_TRUE(builder.write().is_ok());

    const std::uintmax_t original_size =
        std::filesystem::file_size(final_path);

    ASSERT_GT(original_size, 1U);

    std::filesystem::resize_file(
        final_path,
        original_size - 1
    );

    Arena arena;

    auto load_result = SSTable::load(final_path, arena);

    EXPECT_FALSE(load_result.is_ok());
}

/*
 * Corrupting footer bytes must be detected either by footer CRC/magic
 * validation or by SSTable section-range validation.
 */
TEST(SSTableLoadTest, CorruptedFooterIsRejected)
{
    TemporaryDirectory directory;

    const std::filesystem::path temporary_path =
        directory.path("table.tmp");

    const std::filesystem::path final_path =
        directory.path("table.sst");

    SSTable builder(temporary_path, final_path);

    ASSERT_TRUE(builder.write().is_ok());

    corrupt_first_footer_byte(final_path);

    Arena arena;

    auto load_result = SSTable::load(final_path, arena);

    EXPECT_FALSE(load_result.is_ok());
}

/*
 * A completely unrelated file must not be accepted as an SSTable.
 */
TEST(SSTableLoadTest, GarbageFileIsRejected)
{
    TemporaryDirectory directory;

    const std::filesystem::path path =
        directory.path("garbage.sst");

    write_bytes(
        path,
        "this is not an SSTable and has no valid footer"
    );

    Arena arena;

    auto load_result = SSTable::load(path, arena);

    EXPECT_FALSE(load_result.is_ok());
}

TEST(SSTableGetTest, GetRequiresAFileBackedLoadedTable)
{
    TemporaryDirectory directory;
    SSTable builder(
        directory.path("table.tmp"),
        directory.path("table.sst")
    );

    std::string key_storage = "alpha";
    ArenaEntry key = make_key(key_storage);
    Arena arena;

    Result<std::optional<InternalRecord>> result =
        builder.get(key, arena);

    ASSERT_FALSE(result.is_ok());
    EXPECT_EQ(result.status.code, StatusCode::InvalidState);
}

TEST(SSTableGetTest, RoundTripReturnsNewestVersionAndTombstone)
{
    TemporaryDirectory directory;
    const std::filesystem::path temporary_path =
        directory.path("table.tmp");
    const std::filesystem::path final_path =
        directory.path("table.sst");

    SSTable builder(temporary_path, final_path);

    OwnedInternalRecord alpha_new("alpha", "new", ::Type::Put, 9);
    OwnedInternalRecord alpha_old("alpha", "old", ::Type::Put, 4);
    OwnedInternalRecord omega("omega", "", ::Type::Tombstone, 3);

    ASSERT_TRUE(builder.append_record(alpha_new.record).is_ok());
    ASSERT_TRUE(builder.append_record(alpha_old.record).is_ok());
    ASSERT_TRUE(builder.append_record(omega.record).is_ok());
    ASSERT_TRUE(builder.write().is_ok());

    Arena load_arena;
    Result<SSTable> loaded_result = SSTable::load(final_path, load_arena);
    ASSERT_TRUE(loaded_result.is_ok()) << loaded_result.status.message;
    SSTable loaded = std::move(loaded_result.value);

    std::string alpha_storage = "alpha";
    ArenaEntry alpha_key = make_key(alpha_storage);
    Arena result_arena;

    Result<std::optional<InternalRecord>> alpha =
        loaded.get(alpha_key, result_arena);

    ASSERT_TRUE(alpha.is_ok()) << alpha.status.message;
    ASSERT_TRUE(alpha.value.has_value());
    EXPECT_EQ(alpha.value->type, ::Type::Put);
    EXPECT_EQ(alpha.value->seq_num, 9u);
    EXPECT_EQ(entry_view(alpha.value->key_entry), "alpha");
    EXPECT_EQ(entry_view(alpha.value->value_entry), "new");

    std::string omega_storage = "omega";
    ArenaEntry omega_key = make_key(omega_storage);

    Result<std::optional<InternalRecord>> tombstone =
        loaded.get(omega_key, result_arena);

    ASSERT_TRUE(tombstone.is_ok()) << tombstone.status.message;
    ASSERT_TRUE(tombstone.value.has_value());
    EXPECT_EQ(tombstone.value->type, ::Type::Tombstone);
    EXPECT_EQ(tombstone.value->seq_num, 3u);
    EXPECT_EQ(entry_view(tombstone.value->key_entry), "omega");
    EXPECT_EQ(tombstone.value->value_entry.size, 0u);
}

TEST(SSTableGetTest, MissingKeyReturnsNulloptWithoutArenaAllocation)
{
    TemporaryDirectory directory;
    const std::filesystem::path final_path =
        directory.path("table.sst");

    SSTable builder(directory.path("table.tmp"), final_path);
    OwnedInternalRecord alpha("alpha", "one", ::Type::Put, 2);
    OwnedInternalRecord omega("omega", "two", ::Type::Put, 1);

    ASSERT_TRUE(builder.append_record(alpha.record).is_ok());
    ASSERT_TRUE(builder.append_record(omega.record).is_ok());
    ASSERT_TRUE(builder.write().is_ok());

    Arena load_arena;
    Result<SSTable> loaded_result = SSTable::load(final_path, load_arena);
    ASSERT_TRUE(loaded_result.is_ok()) << loaded_result.status.message;
    SSTable loaded = std::move(loaded_result.value);

    std::string missing_storage = "beta";
    ArenaEntry missing_key = make_key(missing_storage);
    Arena result_arena;
    const std::uint64_t before = arena_used_bytes(result_arena);

    Result<std::optional<InternalRecord>> result =
        loaded.get(missing_key, result_arena);

    ASSERT_TRUE(result.is_ok()) << result.status.message;
    EXPECT_FALSE(result.value.has_value());
    EXPECT_EQ(arena_used_bytes(result_arena), before);
}

TEST(SSTableGetTest, LookupUsesTheIndexedBlockNotAlwaysTheFirstBlock)
{
    TemporaryDirectory directory;
    const std::filesystem::path final_path =
        directory.path("table.sst");

    SSTable builder(directory.path("table.tmp"), final_path);
    std::list<OwnedInternalRecord> records;

    // Each serialized record is large enough that these records span several
    // physical data blocks, while every individual record still fits one block.
    for (std::size_t i = 0; i < 8; ++i) {
        const std::string key =
            "key-00" + std::to_string(i);
        const std::string value(1200, static_cast<char>('a' + i));

        records.emplace_back(
            key,
            value,
            ::Type::Put,
            static_cast<std::uint64_t>(100 - i)
        );
        ASSERT_TRUE(builder.append_record(records.back().record).is_ok());
    }

    ASSERT_TRUE(builder.write().is_ok());

    Arena load_arena;
    Result<SSTable> loaded_result = SSTable::load(final_path, load_arena);
    ASSERT_TRUE(loaded_result.is_ok()) << loaded_result.status.message;
    SSTable loaded = std::move(loaded_result.value);

    ASSERT_GT(loaded.get_data_section_view().data_blocks.size(), 1u);

    std::string key_storage = "key-007";
    ArenaEntry key = make_key(key_storage);
    Arena result_arena;

    Result<std::optional<InternalRecord>> result =
        loaded.get(key, result_arena);

    ASSERT_TRUE(result.is_ok()) << result.status.message;
    ASSERT_TRUE(result.value.has_value());
    EXPECT_EQ(entry_view(result.value->key_entry), "key-007");
    EXPECT_EQ(result.value->seq_num, 93u);
    EXPECT_EQ(result.value->value_entry.size, 1200u);
    EXPECT_EQ(
        static_cast<const char*>(result.value->value_entry.data)[0],
        'h'
    );
}

TEST(SSTableGetTest, InvalidKeyPointerIsRejectedBeforeIndexLookup)
{
    TemporaryDirectory directory;
    const std::filesystem::path final_path =
        directory.path("table.sst");

    SSTable builder(directory.path("table.tmp"), final_path);
    OwnedInternalRecord record("alpha", "value", ::Type::Put, 1);
    ASSERT_TRUE(builder.append_record(record.record).is_ok());
    ASSERT_TRUE(builder.write().is_ok());

    Arena load_arena;
    Result<SSTable> loaded_result = SSTable::load(final_path, load_arena);
    ASSERT_TRUE(loaded_result.is_ok()) << loaded_result.status.message;
    SSTable loaded = std::move(loaded_result.value);

    ArenaEntry invalid_key{};
    invalid_key.data = nullptr;
    invalid_key.size = 1;
    Arena result_arena;

    Result<std::optional<InternalRecord>> result =
        loaded.get(invalid_key, result_arena);

    ASSERT_FALSE(result.is_ok());
    EXPECT_EQ(result.status.code, StatusCode::InvalidArgument);
}

TEST(SSTableGetTest, CorruptedDataPayloadIsDetectedLazily)
{
    TemporaryDirectory directory;
    const std::filesystem::path final_path =
        directory.path("table.sst");

    SSTable builder(directory.path("table.tmp"), final_path);
    OwnedInternalRecord record("alpha", "value", ::Type::Put, 1);
    ASSERT_TRUE(builder.append_record(record.record).is_ok());
    ASSERT_TRUE(builder.write().is_ok());

    std::uint64_t payload_offset = 0;
    {
        Arena inspection_arena;
        Result<SSTable> inspection =
            SSTable::load(final_path, inspection_arena);
        ASSERT_TRUE(inspection.is_ok()) << inspection.status.message;
        ASSERT_FALSE(
            inspection.value.get_data_section_view().data_blocks.empty()
        );
        payload_offset = inspection.value
            .get_data_section_view()
            .data_blocks.front()
            .header_view
            .payload_offset;
    }

    flip_byte_at(final_path, payload_offset);

    Arena load_arena;
    Result<SSTable> loaded_result = SSTable::load(final_path, load_arena);
    ASSERT_TRUE(loaded_result.is_ok()) << loaded_result.status.message;
    SSTable loaded = std::move(loaded_result.value);

    std::string key_storage = "alpha";
    ArenaEntry key = make_key(key_storage);
    Arena result_arena;

    Result<std::optional<InternalRecord>> result =
        loaded.get(key, result_arena);

    ASSERT_FALSE(result.is_ok());
    EXPECT_EQ(result.status.code, StatusCode::ChecksumMismatch);
}
