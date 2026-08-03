#include "benchmark_config.h"
#include "timer.h"

#include "db_options.h"
#include "engine.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr double bytes_per_mib = 1024.0 * 1024.0;

class TemporaryDirectory final
{
public:
    TemporaryDirectory()
    {
        const auto nonce = std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
        path_ = std::filesystem::temp_directory_path() /
            ("kvdb_engine_benchmark_" + std::to_string(nonce));

        std::error_code error;
        const bool created = std::filesystem::create_directory(path_, error);
        if (!created || error) {
            throw std::runtime_error(
                "could not create benchmark directory " + path_.string() +
                ": " + error.message()
            );
        }
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class RunDirectory final
{
public:
    RunDirectory(
        const std::filesystem::path& source,
        std::filesystem::path destination
    )
        : path_(std::move(destination))
    {
        std::error_code error;
        std::filesystem::copy(
            source,
            path_,
            std::filesystem::copy_options::recursive,
            error
        );
        if (error) {
            throw std::runtime_error(
                "could not copy recovery fixture: " + error.message()
            );
        }
    }

    ~RunDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    RunDirectory(const RunDirectory&) = delete;
    RunDirectory& operator=(const RunDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

struct Fixture
{
    std::filesystem::path directory;
    std::uint64_t wal_bytes = 0;
};

[[nodiscard]] std::runtime_error status_error(
    std::string operation,
    const Status& status
)
{
    return std::runtime_error(
        std::move(operation) + ": " + status.message
    );
}

[[nodiscard]] DBOptions engine_options(
    const std::filesystem::path& path,
    bool create_if_missing
)
{
    DBOptions options;
    options.db_path = path;
    options.create_if_missing = create_if_missing;
    options.error_if_exists = false;
    options.memtable.size_limit =
        std::numeric_limits<std::size_t>::max() / 4;
    options.wal.file_size_limit =
        std::numeric_limits<std::uint64_t>::max() / 4;
    options.wal.sync_on_write = false;
    options.compaction.enable_background_compaction = false;
    return options;
}

[[nodiscard]] Fixture create_fixture(
    const std::filesystem::path& root,
    const kvdb::benchmark::Scenario& scenario
)
{
    const std::filesystem::path directory =
        root / (scenario.name + "_template");
    DBOptions options = engine_options(directory, true);
    Engine engine(std::move(options));

    Status status = engine.open();
    if (!status.is_ok()) {
        throw status_error("could not open engine fixture", status);
    }

    std::string value = kvdb::benchmark::make_value(scenario.value_bytes);
    for (std::size_t index = 0; index < scenario.records; ++index) {
        std::string key = kvdb::benchmark::make_key(
            scenario.key_bytes,
            index
        );
        status = engine.put(key, value);
        if (!status.is_ok()) {
            (void)engine.close();
            throw status_error("could not write engine fixture", status);
        }
    }

    status = engine.close();
    if (!status.is_ok()) {
        throw status_error("could not close engine fixture", status);
    }

    const std::filesystem::path wal_path =
        directory / "wal-000000001.log";
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(wal_path, error);
    if (error || size > std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error(
            "could not determine engine WAL size: " + error.message()
        );
    }
    return Fixture{
        .directory = directory,
        .wal_bytes = static_cast<std::uint64_t>(size)
    };
}

[[nodiscard]] kvdb::benchmark::Timer::Duration recover_once(
    const Fixture& fixture,
    const std::filesystem::path& root,
    const kvdb::benchmark::Scenario& scenario,
    std::size_t run_number
)
{
    RunDirectory run_directory(
        fixture.directory,
        root / (scenario.name + "_run_" + std::to_string(run_number))
    );
    DBOptions options = engine_options(run_directory.path(), false);
    Engine engine(std::move(options));

    kvdb::benchmark::Timer timer;
    Status status = engine.open();
    const kvdb::benchmark::Timer::Duration elapsed = timer.stop();
    if (!status.is_ok()) {
        throw status_error("engine recovery failed", status);
    }

    std::string final_key = kvdb::benchmark::make_key(
        scenario.key_bytes,
        scenario.records - 1
    );
    Result<std::optional<std::string>> recovered = engine.get(final_key);
    if (!recovered.is_ok()) {
        throw status_error("could not validate recovered key", recovered.status);
    }
    if (!recovered.value.has_value() ||
        recovered.value->size() != scenario.value_bytes) {
        throw std::runtime_error("engine recovery returned an invalid value");
    }

    status = engine.close();
    if (!status.is_ok()) {
        throw status_error("could not close recovered engine", status);
    }
    return elapsed;
}

void print_header(const kvdb::benchmark::Options& options)
{
    std::cout
        << "# KVDB end-to-end Engine::open WAL recovery benchmark\n"
        << "# warmup_iterations=" << options.warmup_iterations
        << ", measured_iterations=" << options.measured_iterations << '\n'
        << "# each timed run uses a fresh fixture copy; copying is not timed\n"
        << "# cache_state=warm; copied fixtures are normally resident in the OS "
           "page cache\n";
#ifdef NDEBUG
    std::cout << "# build=optimized (NDEBUG defined)\n";
#else
    std::cout << "# WARNING: debug build; use Release for comparable results\n";
#endif
    std::cout
        << "scenario,records,key_bytes,value_bytes,wal_mib,min_ms,median_ms,"
           "p95_ms,p99_ms,max_ms,mean_ms,stddev_ms,records_per_second,"
           "mib_per_second,"
           "ns_per_record\n";
}

void print_result(
    const kvdb::benchmark::Scenario& scenario,
    const Fixture& fixture,
    const std::vector<kvdb::benchmark::Timer::Duration>& samples
)
{
    const kvdb::benchmark::TimingSummary timing =
        kvdb::benchmark::summarize_timings(samples);
    const double median_seconds = timing.median_ms / 1000.0;
    const double records_per_second =
        static_cast<double>(scenario.records) / median_seconds;
    const double wal_mib =
        static_cast<double>(fixture.wal_bytes) / bytes_per_mib;
    const double mib_per_second = wal_mib / median_seconds;
    const double nanoseconds_per_record = timing.median_ms * 1'000'000.0 /
        static_cast<double>(scenario.records);

    std::cout << std::fixed << std::setprecision(3)
        << scenario.name << ','
        << scenario.records << ','
        << scenario.key_bytes << ','
        << scenario.value_bytes << ','
        << wal_mib << ','
        << timing.minimum_ms << ','
        << timing.median_ms << ','
        << timing.p95_ms << ','
        << timing.p99_ms << ','
        << timing.maximum_ms << ','
        << timing.mean_ms << ','
        << timing.standard_deviation_ms << ','
        << records_per_second << ','
        << mib_per_second << ','
        << nanoseconds_per_record << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const kvdb::benchmark::Options options =
            kvdb::benchmark::parse_options(argc, argv);
        if (options.show_help) {
            kvdb::benchmark::print_usage(std::cout, argv[0]);
            return 0;
        }

        const std::vector<kvdb::benchmark::Scenario> scenarios =
            kvdb::benchmark::make_scenarios(options);
        TemporaryDirectory temporary_directory;
        std::size_t run_number = 0;
        print_header(options);

        for (const kvdb::benchmark::Scenario& scenario : scenarios) {
            const Fixture fixture = create_fixture(
                temporary_directory.path(),
                scenario
            );

            for (std::size_t index = 0;
                index < options.warmup_iterations;
                ++index) {
                (void)recover_once(
                    fixture,
                    temporary_directory.path(),
                    scenario,
                    run_number++
                );
            }

            std::vector<kvdb::benchmark::Timer::Duration> samples;
            samples.reserve(options.measured_iterations);
            for (std::size_t index = 0;
                index < options.measured_iterations;
                ++index) {
                samples.push_back(recover_once(
                    fixture,
                    temporary_directory.path(),
                    scenario,
                    run_number++
                ));
            }
            print_result(scenario, fixture, samples);
        }
    }
    catch (const std::exception& exception) {
        std::cerr << "benchmark failed: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}
