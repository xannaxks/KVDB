#include "manifest.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

    class TempManifestPath {
    public:
        TempManifestPath()
        {
            const auto nonce = std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();
            path_ = std::filesystem::temp_directory_path() /
                ("kvdb-manifest-test-" + std::to_string(nonce));
        }

        ~TempManifestPath()
        {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
        }

        const std::filesystem::path& get() const noexcept
        {
            return path_;
        }

    private:
        std::filesystem::path path_;
    };

    // Adapt only this helper if your ArenaEntry constructor takes another pointer
    // or size type.
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
            std::string_view largest
        )
        {
            TableMeta result{};
            result.table_id = id;
            result.level = level;
            result.smallest_key = key(smallest);
            result.largest_key = key(largest);
            result.file_size = 100;
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

    void append_bytes(
        const std::filesystem::path& path,
        std::initializer_list<unsigned char> bytes
    )
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        ASSERT_TRUE(out.is_open());
        for (unsigned char byte : bytes) {
            out.put(static_cast<char>(byte));
        }
        ASSERT_TRUE(out.good());
    }

    void flip_byte(
        const std::filesystem::path& path,
        std::uint64_t offset
    )
    {
        std::fstream file(
            path,
            std::ios::binary | std::ios::in | std::ios::out
        );
        ASSERT_TRUE(file.is_open());

        file.seekg(static_cast<std::streamoff>(offset));
        char value = 0;
        file.get(value);
        ASSERT_TRUE(file.good());

        value ^= static_cast<char>(0x01);
        file.seekp(static_cast<std::streamoff>(offset));
        file.put(value);
        ASSERT_TRUE(file.good());
    }

    TEST(VersionEditTest, PrepareDerivesPayloadSizeAndCrc)
    {
        VersionEdit edit;
        edit.payload.next_table_id = 9;
        edit.payload.next_sequence_number = 100;
        edit.payload.current_wal_id = 4;

        ASSERT_STATUS_OK(edit.prepare());

        auto size = edit.payload.encoded_size();
        ASSERT_TRUE(size.is_ok()) << size.status.message;
        EXPECT_EQ(edit.header.payload_size, size.value);

        std::uint32_t crc = 0;
        ASSERT_STATUS_OK(edit.payload.compute_crc32(crc));
        EXPECT_EQ(edit.header.crc32, crc);
    }

    TEST(ManifestApplyTest, FailedEditLeavesLevelsAndCountersUnchanged)
    {
        TempManifestPath path;
        Manifest manifest(path.get());
        LevelManager levels(2);
        MetaFactory factory;

        VersionEdit initial;
        initial.payload.new_tables.push_back(
            factory.table(1, 1, "a", "m")
        );
        ASSERT_STATUS_OK(manifest.apply(levels, initial));
        ASSERT_EQ(manifest.next_table_id(), 2u);

        VersionEdit invalid;
        invalid.payload.deleted_tables.push_back(DeletedTable{ 1, 1 });
        invalid.payload.new_tables.push_back(
            factory.table(2, 9, "n", "z")
        );
        invalid.payload.next_table_id = 50;

        Status status = manifest.apply(levels, invalid);

        EXPECT_FALSE(status.is_ok());
        ASSERT_EQ(levels.levels(1).size(), 1u);
        EXPECT_EQ(levels.levels(1)[0].table_id, 1u);
        EXPECT_EQ(manifest.next_table_id(), 2u);
    }

    TEST(ManifestApplyTest, RejectsCounterRegressionAtomically)
    {
        TempManifestPath path;
        Manifest manifest(path.get());
        LevelManager levels(2);

        VersionEdit advance;
        advance.payload.next_table_id = 10;
        advance.payload.next_sequence_number = 20;
        advance.payload.current_wal_id = 3;
        ASSERT_STATUS_OK(manifest.apply(levels, advance));

        VersionEdit regress;
        regress.payload.next_table_id = 9;
        regress.payload.next_sequence_number = 19;
        regress.payload.current_wal_id = 2;

        Status status = manifest.apply(levels, regress);

        EXPECT_FALSE(status.is_ok());
        EXPECT_EQ(manifest.next_table_id(), 10u);
        EXPECT_EQ(manifest.next_sequence_number(), 20u);
        EXPECT_EQ(manifest.current_wal_id(), 3u);
    }

    TEST(ManifestApplyTest, NextSequenceGetterReturnsNextNotPrevious)
    {
        TempManifestPath path;
        Manifest manifest(path.get());
        LevelManager levels(1);

        VersionEdit edit;
        edit.payload.next_sequence_number = 42;
        ASSERT_STATUS_OK(manifest.apply(levels, edit));

        EXPECT_EQ(manifest.next_sequence_number(), 42u);
    }

    TEST(ManifestCommitTest, InvalidEditIsRejectedBeforeFileChanges)
    {
        TempManifestPath path;
        Manifest manifest(path.get());
        LevelManager levels(1);

        ASSERT_STATUS_OK(manifest.open_or_create());
        const auto before = std::filesystem::file_size(path.get());

        VersionEdit invalid;
        invalid.payload.next_table_id = 0;

        Status status = manifest.commit(levels, invalid);

        EXPECT_FALSE(status.is_ok());
        EXPECT_EQ(std::filesystem::file_size(path.get()), before);
        EXPECT_FALSE(manifest.write_poisoned());
    }

    TEST(ManifestRecoveryTest, RoundTripsScalarState)
    {
        TempManifestPath path;

        {
            Manifest writer(path.get());
            LevelManager levels(2);
            ASSERT_STATUS_OK(writer.open_or_create());

            VersionEdit edit;
            edit.payload.next_table_id = 17;
            edit.payload.next_sequence_number = 91;
            edit.payload.current_wal_id = 6;
            ASSERT_STATUS_OK(writer.commit(levels, edit));
        }

        LevelManager recovered_levels(2);
        Arena arena;
        auto loaded = Manifest::load(recovered_levels, path.get(), arena);

        ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;
        EXPECT_EQ(loaded.value.next_table_id(), 17u);
        EXPECT_EQ(loaded.value.next_sequence_number(), 91u);
        EXPECT_EQ(loaded.value.current_wal_id(), 6u);
        EXPECT_FALSE(loaded.value.has_recoverable_tail());
    }

    TEST(ManifestRecoveryTest, IgnoresOnlyIncompleteFinalRecord)
    {
        TempManifestPath path;
        std::uint64_t clean_size = 0;

        {
            Manifest writer(path.get());
            LevelManager levels(1);
            ASSERT_STATUS_OK(writer.open_or_create());

            VersionEdit edit;
            edit.payload.next_table_id = 5;
            ASSERT_STATUS_OK(writer.commit(levels, edit));
            clean_size = writer.append_offset();
        }

        append_bytes(path.get(), { 0x11, 0x22, 0x33 });
        ASSERT_GT(std::filesystem::file_size(path.get()), clean_size);

        LevelManager recovered_levels(1);
        Arena arena;
        auto loaded = Manifest::load(recovered_levels, path.get(), arena);

        ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;
        EXPECT_TRUE(loaded.value.has_recoverable_tail());
        EXPECT_EQ(loaded.value.append_offset(), clean_size);
        EXPECT_EQ(loaded.value.next_table_id(), 5u);

        ASSERT_STATUS_OK(loaded.value.prepare_for_append());
        EXPECT_FALSE(loaded.value.has_recoverable_tail());
        EXPECT_EQ(std::filesystem::file_size(path.get()), clean_size);
    }

    TEST(ManifestRecoveryTest, CompleteRecordWithBadCrcIsCorruption)
    {
        TempManifestPath path;

        {
            Manifest writer(path.get());
            LevelManager levels(1);
            ASSERT_STATUS_OK(writer.open_or_create());

            VersionEdit edit;
            edit.payload.next_table_id = 7;
            ASSERT_STATUS_OK(writer.commit(levels, edit));
        }

        // Header: 24 bytes. Edit header: 8 bytes. Payload flags: 4 bytes.
        // Flip one byte in the encoded next_table_id while leaving the record
        // complete and structurally parseable.
        flip_byte(
            path.get(),
            Manifest::Header::disk_size() +
            VersionEdit::Header::disk_size() +
            4u
        );

        LevelManager destination(1);
        Arena arena;
        auto loaded = Manifest::load(destination, path.get(), arena);

        ASSERT_FALSE(loaded.is_ok());
        EXPECT_EQ(loaded.status.code, StatusCode::ChecksumMismatch);
        EXPECT_TRUE(destination.empty());
    }

    TEST(ManifestRecoveryTest, FailedLoadDoesNotModifyCallerLevelManager)
    {
        TempManifestPath path;
        MetaFactory factory;

        {
            Manifest writer(path.get());
            LevelManager levels(2);
            ASSERT_STATUS_OK(writer.open_or_create());

            VersionEdit edit;
            edit.payload.next_table_id = 7;
            ASSERT_STATUS_OK(writer.commit(levels, edit));
        }

        flip_byte(
            path.get(),
            Manifest::Header::disk_size() +
            VersionEdit::Header::disk_size() +
            4u
        );

        LevelManager destination(2);
        ASSERT_STATUS_OK(destination.add_table(
            factory.table(1, 1, "a", "z")
        ));

        Arena arena;
        auto loaded = Manifest::load(destination, path.get(), arena);

        ASSERT_FALSE(loaded.is_ok());
        ASSERT_EQ(destination.levels(1).size(), 1u);
        EXPECT_EQ(destination.levels(1)[0].table_id, 1u);
    }

    TEST(ManifestRecoveryTest, RoundTripsTableMetadataAndOrdering)
    {
        TempManifestPath path;
        MetaFactory factory;

        {
            Manifest writer(path.get());
            LevelManager levels(3);
            ASSERT_STATUS_OK(writer.open_or_create());

            VersionEdit edit;
            edit.payload.new_tables.push_back(
                factory.table(3, 0, "a", "m")
            );
            edit.payload.new_tables.push_back(
                factory.table(8, 0, "f", "z")
            );
            edit.payload.new_tables.push_back(
                factory.table(4, 1, "a", "f")
            );
            edit.payload.new_tables.push_back(
                factory.table(5, 1, "g", "z")
            );
            ASSERT_STATUS_OK(writer.commit(levels, edit));
        }

        LevelManager recovered(3);
        Arena arena;
        auto loaded = Manifest::load(recovered, path.get(), arena);

        ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;
        ASSERT_EQ(recovered.levels(0).size(), 2u);
        EXPECT_EQ(recovered.levels(0)[0].table_id, 8u);
        EXPECT_EQ(recovered.levels(0)[1].table_id, 3u);

        ASSERT_EQ(recovered.levels(1).size(), 2u);
        EXPECT_EQ(recovered.levels(1)[0].table_id, 4u);
        EXPECT_EQ(recovered.levels(1)[1].table_id, 5u);
        EXPECT_EQ(loaded.value.next_table_id(), 9u);
    }

} // namespace