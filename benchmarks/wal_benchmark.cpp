#include "benchmark_config.h"
#include "timer.h"

#include "arena.h"
#include "record.h"
#include "wal.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t benchmark_wal_id = 1;
constexpr std::uint64_t first_sequence = 1;
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
            ("kvdb_wal_benchmark_" + std::to_string(nonce));

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

struct Fixture
{
    std::filesystem::path path;
    std::uint64_t wal_bytes = 0;
};

struct RecoverySample
{
    kvdb::benchmark::Timer::Duration elapsed{};
    std::uint64_t arena_used_bytes = 0;
    std::uint64_t arena_reserved_bytes = 0;
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

[[nodiscard]] Fixture create_fixture(
    const std::filesystem::path& directory,
    const kvdb::benchmark::Scenario& scenario
)
{
    const std::filesystem::path path =
        directory / (scenario.name + ".wal");
    const std::string value = kvdb::benchmark::make_value(
        scenario.value_bytes
    );

    WALWriter writer;
    Status status = writer.create(
        path,
        benchmark_wal_id,
        first_sequence
    );
    if (!status.is_ok()) {
        throw status_error("could not create WAL fixture", status);
    }

    for (std::size_t index = 0; index < scenario.records; ++index) {
        std::string key = kvdb::benchmark::make_key(
            scenario.key_bytes,
            index
        );
        const ArenaEntry key_entry(key.data(), key.size());
        const ArenaEntry value_entry(value.data(), value.size());
        const std::uint64_t sequence = first_sequence +
            static_cast<std::uint64_t>(index);
        const InternalRecord record(
            key_entry,
            value_entry,
            Type::Put,
            sequence
        );

        status = writer.write(record);
        if (!status.is_ok()) {
            (void)writer.close();
            throw status_error("could not write WAL fixture", status);
        }
    }

    status = writer.close();
    if (!status.is_ok()) {
        throw status_error("could not close WAL fixture", status);
    }

    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error || size > std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error(
            "could not determine WAL fixture size: " + error.message()
        );
    }

    return Fixture{
        .path = path,
        .wal_bytes = static_cast<std::uint64_t>(size)
    };
}

[[nodiscard]] RecoverySample recover_once(
    const Fixture& fixture,
    const kvdb::benchmark::Scenario& scenario
)
{
    Arena arena;
    kvdb::benchmark::Timer timer;
    Result<WALLoader::LoadResult> recovered = WAL::recover(
        fixture.path,
        benchmark_wal_id,
        arena
    );
    const kvdb::benchmark::Timer::Duration elapsed = timer.stop();

    if (!recovered.is_ok()) {
        throw status_error("WAL recovery failed", recovered.status);
    }
    if (!recovered.value.ok || recovered.value.had_corruption ||
        recovered.value.had_torn_tail) {
        throw std::runtime_error(
            "WAL recovery did not report a complete valid fixture: " +
            recovered.value.error
        );
    }
    if (recovered.value.records.size() != scenario.records) {
        throw std::runtime_error("WAL recovery returned the wrong record count");
    }
    if (recovered.value.records.empty() ||
        recovered.value.records.front().seq_num != first_sequence ||
        recovered.value.records.back().seq_num !=
            first_sequence + static_cast<std::uint64_t>(scenario.records - 1)) {
        throw std::runtime_error("WAL recovery returned invalid sequence numbers");
    }

    const Result<std::uint64_t> used = arena.get_used_bytes();
    if (!used.is_ok()) {
        throw status_error("could not read arena used bytes", used.status);
    }
    const Result<std::uint64_t> reserved = arena.get_reserved_bytes();
    if (!reserved.is_ok()) {
        throw status_error(
            "could not read arena reserved bytes",
            reserved.status
        );
    }

    return RecoverySample{
        .elapsed = elapsed,
        .arena_used_bytes = used.value,
        .arena_reserved_bytes = reserved.value
    };
}

void print_header(const kvdb::benchmark::Options& options)
{
    std::cout
        << "# KVDB isolated WAL::recover benchmark\n"
        << "# warmup_iterations=" << options.warmup_iterations
        << ", measured_iterations=" << options.measured_iterations << '\n'
        << "# cache_state=warm; fixture creation and warmups happen before "
           "measurement\n";
#ifdef NDEBUG
    std::cout << "# build=optimized (NDEBUG defined)\n";
#else
    std::cout << "# WARNING: debug build; use Release for comparable results\n";
#endif
    std::cout
        << "scenario,records,key_bytes,value_bytes,wal_mib,min_ms,median_ms,"
           "p95_ms,p99_ms,max_ms,mean_ms,stddev_ms,records_per_second,"
           "mib_per_second,"
           "ns_per_record,arena_used_mib,arena_reserved_mib\n";
}

void print_result(
    const kvdb::benchmark::Scenario& scenario,
    const Fixture& fixture,
    const std::vector<RecoverySample>& samples
)
{
    std::vector<kvdb::benchmark::Timer::Duration> durations;
    durations.reserve(samples.size());
    for (const RecoverySample& sample : samples) {
        durations.push_back(sample.elapsed);
    }

    const kvdb::benchmark::TimingSummary timing =
        kvdb::benchmark::summarize_timings(durations);
    const double median_seconds = timing.median_ms / 1000.0;
    const double records_per_second =
        static_cast<double>(scenario.records) / median_seconds;
    const double wal_mib =
        static_cast<double>(fixture.wal_bytes) / bytes_per_mib;
    const double mib_per_second = wal_mib / median_seconds;
    const double nanoseconds_per_record = timing.median_ms * 1'000'000.0 /
        static_cast<double>(scenario.records);
    const RecoverySample& memory = samples.back();

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
        << nanoseconds_per_record << ','
        << static_cast<double>(memory.arena_used_bytes) / bytes_per_mib << ','
        << static_cast<double>(memory.arena_reserved_bytes) / bytes_per_mib
        << '\n';
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
        print_header(options);

        for (const kvdb::benchmark::Scenario& scenario : scenarios) {
            const Fixture fixture = create_fixture(
                temporary_directory.path(),
                scenario
            );

            for (std::size_t index = 0;
                index < options.warmup_iterations;
                ++index) {
                (void)recover_once(fixture, scenario);
            }

            std::vector<RecoverySample> samples;
            samples.reserve(options.measured_iterations);
            for (std::size_t index = 0;
                index < options.measured_iterations;
                ++index) {
                samples.push_back(recover_once(fixture, scenario));
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
