#include <gtest/gtest.h>

#include "kvdb.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace
{
    class TemporaryDatabaseDirectory
    {
    public:
        TemporaryDatabaseDirectory()
        {
            static std::atomic<std::uint64_t> next_id{ 0 };
            const auto timestamp = std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();

            path_ = std::filesystem::temp_directory_path() /
                ("kvdb_engine_test_" +
                    std::to_string(timestamp) + "_" +
                    std::to_string(next_id.fetch_add(1)));
        }

        TemporaryDatabaseDirectory(
            const TemporaryDatabaseDirectory&
        ) = delete;
        TemporaryDatabaseDirectory& operator=(
            const TemporaryDatabaseDirectory&
        ) = delete;

        ~TemporaryDatabaseDirectory()
        {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }

        [[nodiscard]] const std::filesystem::path& path() const noexcept
        {
            return path_;
        }

    private:
        std::filesystem::path path_;
    };

    [[nodiscard]] DBOptions test_options(
        const std::filesystem::path& path
    )
    {
        DBOptions options;
        options.db_path = path;
        options.compaction.enable_background_compaction = false;
        return options;
    }

    void expect_value(
        KVDB& database,
        std::string_view key,
        std::string_view expected
    )
    {
        Result<std::optional<std::string>> result = database.get(key);
        ASSERT_TRUE(result.is_ok()) << result.status.message;
        ASSERT_TRUE(result.value.has_value());
        EXPECT_EQ(*result.value, expected);
    }

    void expect_missing(KVDB& database, std::string_view key)
    {
        Result<std::optional<std::string>> result = database.get(key);
        ASSERT_TRUE(result.is_ok()) << result.status.message;
        EXPECT_FALSE(result.value.has_value());
    }

    void put(KVDB& database, std::string key, std::string value)
    {
        const Status status = database.put(key, value);
        ASSERT_TRUE(status.is_ok()) << status.message;
    }
}

TEST(EngineIntegrationTest, FlushAndReopenPreserveValuesAndTombstones)
{
    TemporaryDatabaseDirectory directory;
    const DBOptions options = test_options(directory.path());

    Result<std::unique_ptr<KVDB>> opened = KVDB::open(options);
    ASSERT_TRUE(opened.is_ok()) << opened.status.message;
    std::unique_ptr<KVDB> database = std::move(opened.value);

    put(*database, "alpha", "one");
    put(*database, "beta", "two");
    put(*database, "empty", "");

    expect_value(*database, "alpha", "one");
    expect_missing(*database, "missing");

    Status status = database->flush();
    ASSERT_TRUE(status.is_ok()) << status.message;

    expect_value(*database, "alpha", "one");
    expect_value(*database, "beta", "two");
    expect_value(*database, "empty", "");
    expect_missing(*database, "missing");

    status = database->close();
    ASSERT_TRUE(status.is_ok()) << status.message;
    database.reset();

    opened = KVDB::open(options);
    ASSERT_TRUE(opened.is_ok()) << opened.status.message;
    database = std::move(opened.value);

    expect_value(*database, "alpha", "one");
    expect_value(*database, "beta", "two");
    expect_value(*database, "empty", "");

    std::string alpha = "alpha";
    status = database->remove(alpha);
    ASSERT_TRUE(status.is_ok()) << status.message;
    expect_missing(*database, "alpha");

    status = database->flush();
    ASSERT_TRUE(status.is_ok()) << status.message;
    expect_missing(*database, "alpha");
    expect_value(*database, "beta", "two");

    status = database->close();
    ASSERT_TRUE(status.is_ok()) << status.message;
    database.reset();

    opened = KVDB::open(options);
    ASSERT_TRUE(opened.is_ok()) << opened.status.message;
    database = std::move(opened.value);

    expect_missing(*database, "alpha");
    expect_value(*database, "beta", "two");

    status = database->close();
    EXPECT_TRUE(status.is_ok()) << status.message;
}

TEST(EngineIntegrationTest, ReopenRecoversUnflushedWal)
{
    TemporaryDatabaseDirectory directory;
    const DBOptions options = test_options(directory.path());

    Result<std::unique_ptr<KVDB>> opened = KVDB::open(options);
    ASSERT_TRUE(opened.is_ok()) << opened.status.message;
    std::unique_ptr<KVDB> database = std::move(opened.value);

    put(*database, "durable", "from-wal");
    Status status = database->close();
    ASSERT_TRUE(status.is_ok()) << status.message;
    database.reset();

    opened = KVDB::open(options);
    ASSERT_TRUE(opened.is_ok()) << opened.status.message;
    database = std::move(opened.value);

    expect_value(*database, "durable", "from-wal");

    status = database->close();
    EXPECT_TRUE(status.is_ok()) << status.message;
}

TEST(EngineIntegrationTest, BackgroundCompactionKeepsNewestVisibleState)
{
    TemporaryDatabaseDirectory directory;
    DBOptions options = test_options(directory.path());
    options.compaction.enable_background_compaction = true;
    options.compaction.l0_file_count_trigger = 2;

    Result<std::unique_ptr<KVDB>> opened = KVDB::open(options);
    ASSERT_TRUE(opened.is_ok()) << opened.status.message;
    std::unique_ptr<KVDB> database = std::move(opened.value);

    put(*database, "alpha", "old");
    put(*database, "stable", "unchanged");
    Status status = database->flush();
    ASSERT_TRUE(status.is_ok()) << status.message;

    put(*database, "alpha", "new");
    put(*database, "beta", "second");
    status = database->flush();
    ASSERT_TRUE(status.is_ok()) << status.message;

    expect_value(*database, "alpha", "new");
    expect_value(*database, "beta", "second");
    expect_value(*database, "stable", "unchanged");

    std::string alpha = "alpha";
    status = database->remove(alpha);
    ASSERT_TRUE(status.is_ok()) << status.message;
    status = database->flush();
    ASSERT_TRUE(status.is_ok()) << status.message;

    put(*database, "gamma", "third");
    status = database->flush();
    ASSERT_TRUE(status.is_ok()) << status.message;

    expect_missing(*database, "alpha");
    expect_value(*database, "beta", "second");
    expect_value(*database, "gamma", "third");
    expect_value(*database, "stable", "unchanged");

    status = database->close();
    ASSERT_TRUE(status.is_ok()) << status.message;
    database.reset();

    opened = KVDB::open(options);
    ASSERT_TRUE(opened.is_ok()) << opened.status.message;
    database = std::move(opened.value);

    expect_missing(*database, "alpha");
    expect_value(*database, "beta", "second");
    expect_value(*database, "gamma", "third");
    expect_value(*database, "stable", "unchanged");

    status = database->close();
    EXPECT_TRUE(status.is_ok()) << status.message;
}

TEST(EngineIntegrationTest, ManualCompactionRoundTripsThroughReopen)
{
    TemporaryDatabaseDirectory directory;
    const DBOptions options = test_options(directory.path());

    Result<std::unique_ptr<KVDB>> opened = KVDB::open(options);
    ASSERT_TRUE(opened.is_ok()) << opened.status.message;
    std::unique_ptr<KVDB> database = std::move(opened.value);

    put(*database, "alpha", "remove-me");
    put(*database, "beta", "old");
    Status status = database->flush();
    ASSERT_TRUE(status.is_ok()) << status.message;

    std::string alpha = "alpha";
    status = database->remove(alpha);
    ASSERT_TRUE(status.is_ok()) << status.message;
    put(*database, "beta", "new");
    put(*database, "omega", "last");
    status = database->flush();
    ASSERT_TRUE(status.is_ok()) << status.message;

    status = database->compact_range("a", "z");
    ASSERT_TRUE(status.is_ok()) << status.message;

    expect_missing(*database, "alpha");
    expect_value(*database, "beta", "new");
    expect_value(*database, "omega", "last");

    status = database->close();
    ASSERT_TRUE(status.is_ok()) << status.message;
    database.reset();

    opened = KVDB::open(options);
    ASSERT_TRUE(opened.is_ok()) << opened.status.message;
    database = std::move(opened.value);

    expect_missing(*database, "alpha");
    expect_value(*database, "beta", "new");
    expect_value(*database, "omega", "last");

    status = database->close();
    EXPECT_TRUE(status.is_ok()) << status.message;
}

TEST(EngineConcurrencyTest, ConcurrentClientsAndFlushesLoseNoWrites)
{
    TemporaryDatabaseDirectory directory;
    const DBOptions options = test_options(directory.path());

    Result<std::unique_ptr<KVDB>> opened = KVDB::open(options);
    ASSERT_TRUE(opened.is_ok()) << opened.status.message;
    std::unique_ptr<KVDB> database = std::move(opened.value);

    put(*database, "anchor", "stable");
    Status status = database->flush();
    ASSERT_TRUE(status.is_ok()) << status.message;

    constexpr int writer_count = 4;
    constexpr int reader_count = 3;
    constexpr int writes_per_writer = 25;
    constexpr int total_writes = writer_count * writes_per_writer;
    constexpr int flush_count = 8;

    std::atomic<bool> start{ false };
    std::atomic<bool> failed{ false };
    std::atomic<int> completed_writes{ 0 };
    std::vector<std::thread> workers;
    workers.reserve(writer_count + reader_count + 1);

    for (int writer_id = 0; writer_id < writer_count; ++writer_id) {
        workers.emplace_back([&, writer_id] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int item = 0;
                item < writes_per_writer &&
                    !failed.load(std::memory_order_acquire);
                ++item) {
                std::string key =
                    "writer-" + std::to_string(writer_id) +
                    "-key-" + std::to_string(item);
                std::string value =
                    "value-" + std::to_string(writer_id) +
                    "-" + std::to_string(item);

                const Status write_status = database->put(key, value);
                if (!write_status.is_ok()) {
                    failed.store(true, std::memory_order_release);
                    break;
                }
                completed_writes.fetch_add(1, std::memory_order_release);
            }
        });
    }

    for (int reader_id = 0; reader_id < reader_count; ++reader_id) {
        workers.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            while (completed_writes.load(std::memory_order_acquire) <
                    total_writes &&
                !failed.load(std::memory_order_acquire)) {
                Result<std::optional<std::string>> result =
                    database->get("anchor");
                if (!result.is_ok() ||
                    !result.value.has_value() ||
                    *result.value != "stable") {
                    failed.store(true, std::memory_order_release);
                    break;
                }
            }
        });
    }

    workers.emplace_back([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (int flush_index = 1;
            flush_index <= flush_count &&
                !failed.load(std::memory_order_acquire);
            ++flush_index) {
            const int milestone =
                (total_writes * flush_index) / flush_count;
            while (completed_writes.load(std::memory_order_acquire) <
                    milestone &&
                !failed.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            if (failed.load(std::memory_order_acquire)) {
                break;
            }

            const Status flush_status = database->flush();
            if (!flush_status.is_ok()) {
                failed.store(true, std::memory_order_release);
                break;
            }
        }
    });

    start.store(true, std::memory_order_release);
    for (std::thread& worker : workers) {
        worker.join();
    }

    ASSERT_FALSE(failed.load(std::memory_order_acquire));
    ASSERT_EQ(
        completed_writes.load(std::memory_order_acquire),
        total_writes
    );

    status = database->flush();
    ASSERT_TRUE(status.is_ok()) << status.message;

    for (int writer_id = 0; writer_id < writer_count; ++writer_id) {
        for (int item = 0; item < writes_per_writer; ++item) {
            const std::string key =
                "writer-" + std::to_string(writer_id) +
                "-key-" + std::to_string(item);
            const std::string expected =
                "value-" + std::to_string(writer_id) +
                "-" + std::to_string(item);
            expect_value(*database, key, expected);
        }
    }

    status = database->close();
    ASSERT_TRUE(status.is_ok()) << status.message;
    database.reset();

    opened = KVDB::open(options);
    ASSERT_TRUE(opened.is_ok()) << opened.status.message;
    database = std::move(opened.value);

    expect_value(*database, "anchor", "stable");
    for (int writer_id = 0; writer_id < writer_count; ++writer_id) {
        for (int item = 0; item < writes_per_writer; ++item) {
            const std::string key =
                "writer-" + std::to_string(writer_id) +
                "-key-" + std::to_string(item);
            const std::string expected =
                "value-" + std::to_string(writer_id) +
                "-" + std::to_string(item);
            expect_value(*database, key, expected);
        }
    }

    status = database->close();
    EXPECT_TRUE(status.is_ok()) << status.message;
}

TEST(
    EngineConcurrencyTest,
    ConcurrentReadsNeverRegressDuringFlushAndCompaction
)
{
    TemporaryDatabaseDirectory directory;
    DBOptions options = test_options(directory.path());
    options.compaction.enable_background_compaction = true;
    options.compaction.l0_file_count_trigger = 3;

    Result<std::unique_ptr<KVDB>> opened = KVDB::open(options);
    ASSERT_TRUE(opened.is_ok()) << opened.status.message;
    std::unique_ptr<KVDB> database = std::move(opened.value);

    put(*database, "hot", "0");

    constexpr int final_version = 200;
    constexpr int reader_count = 4;
    constexpr int flush_count = 12;

    std::atomic<bool> start{ false };
    std::atomic<bool> writer_done{ false };
    std::atomic<bool> failed{ false };
    std::atomic<int> written_version{ 0 };
    std::vector<std::thread> threads;
    threads.reserve(reader_count + 2);

    threads.emplace_back([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (int version = 1;
            version <= final_version &&
                !failed.load(std::memory_order_acquire);
            ++version) {
            std::string key = "hot";
            std::string value = std::to_string(version);
            const Status write_status = database->put(key, value);
            if (!write_status.is_ok()) {
                failed.store(true, std::memory_order_release);
                break;
            }
            written_version.store(version, std::memory_order_release);
        }

        writer_done.store(true, std::memory_order_release);
    });

    for (int reader_id = 0; reader_id < reader_count; ++reader_id) {
        threads.emplace_back([&] {
            int last_seen = 0;

            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            while (!writer_done.load(std::memory_order_acquire) &&
                !failed.load(std::memory_order_acquire)) {
                Result<std::optional<std::string>> result =
                    database->get("hot");
                if (!result.is_ok() || !result.value.has_value()) {
                    failed.store(true, std::memory_order_release);
                    break;
                }

                int observed = 0;
                try {
                    observed = std::stoi(*result.value);
                }
                catch (...) {
                    failed.store(true, std::memory_order_release);
                    break;
                }

                if (observed < last_seen ||
                    observed < 0 ||
                    observed > final_version) {
                    failed.store(true, std::memory_order_release);
                    break;
                }
                last_seen = observed;
            }
        });
    }

    threads.emplace_back([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        for (int flush_index = 1;
            flush_index <= flush_count &&
                !failed.load(std::memory_order_acquire);
            ++flush_index) {
            const int milestone =
                (final_version * flush_index) / flush_count;
            while (written_version.load(std::memory_order_acquire) <
                    milestone &&
                !writer_done.load(std::memory_order_acquire) &&
                !failed.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            if (failed.load(std::memory_order_acquire)) {
                break;
            }

            const Status flush_status = database->flush();
            if (!flush_status.is_ok()) {
                failed.store(true, std::memory_order_release);
                break;
            }
        }
    });

    start.store(true, std::memory_order_release);
    for (std::thread& thread : threads) {
        thread.join();
    }

    ASSERT_FALSE(failed.load(std::memory_order_acquire));
    ASSERT_EQ(
        written_version.load(std::memory_order_acquire),
        final_version
    );

    Status status = database->flush();
    ASSERT_TRUE(status.is_ok()) << status.message;
    expect_value(
        *database,
        "hot",
        std::to_string(final_version)
    );

    status = database->close();
    ASSERT_TRUE(status.is_ok()) << status.message;
    database.reset();

    opened = KVDB::open(options);
    ASSERT_TRUE(opened.is_ok()) << opened.status.message;
    database = std::move(opened.value);

    expect_value(
        *database,
        "hot",
        std::to_string(final_version)
    );

    status = database->close();
    EXPECT_TRUE(status.is_ok()) << status.message;
}
