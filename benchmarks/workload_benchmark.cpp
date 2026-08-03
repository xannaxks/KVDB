#include "benchmark_config.h"
#include "process_io.h"
#include "timer.h"

#include "db_options.h"
#include "engine.h"
#include "status.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view all_workloads = "all";
constexpr std::string_view sequential_insert = "sequential_insert";
constexpr std::string_view random_insert = "random_insert";
constexpr std::string_view random_read = "random_read";
constexpr std::string_view missing_read = "missing_read";
constexpr std::string_view mixed_95_5 = "mixed_95_5";
constexpr std::string_view wal_recovery = "wal_recovery";
constexpr std::string_view flush_compaction = "flush_compaction";

struct Options
{
    std::string workload{all_workloads};
    std::size_t operations = 10'000;
    std::size_t dataset_records = 200;
    std::size_t maintenance_operations = 200;
    std::uint32_t key_bytes = 16;
    std::uint32_t value_bytes = 256;
    std::uint64_t seed = 0x4b564442u;
    std::size_t recovery_iterations = 15;
    bool show_help = false;
};

struct Metrics
{
    std::string workload;
    std::size_t dataset_records = 0;
    std::size_t operation_count = 0;
    double operations_per_second = 0.0;
    kvdb::benchmark::TimingSummary latency{};
    std::uint64_t bytes_written = 0;
    std::uint64_t flush_count = 0;
    std::uint64_t compaction_count = 0;
};

class TemporaryDirectory final
{
public:
    TemporaryDirectory()
    {
        const auto nonce = std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
        path_ = std::filesystem::temp_directory_path() /
            ("kvdb_workload_benchmark_" + std::to_string(nonce));

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

[[nodiscard]] std::runtime_error status_error(
    std::string operation,
    const Status& status
)
{
    return std::runtime_error(
        std::move(operation) + ": " + status.message
    );
}

[[nodiscard]] std::uint64_t parse_unsigned(
    std::string_view option,
    std::string_view text
)
{
    std::uint64_t value = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        throw std::invalid_argument(
            std::string(option) + " expects a non-negative integer"
        );
    }
    return value;
}

[[nodiscard]] std::string_view require_value(
    int argc,
    char** argv,
    int& index,
    std::string_view option
)
{
    ++index;
    if (index >= argc) {
        throw std::invalid_argument(
            std::string(option) + " requires a value"
        );
    }
    return argv[index];
}

[[nodiscard]] bool valid_workload(std::string_view workload)
{
    return workload == all_workloads ||
        workload == sequential_insert ||
        workload == random_insert ||
        workload == random_read ||
        workload == missing_read ||
        workload == mixed_95_5 ||
        workload == wal_recovery ||
        workload == flush_compaction;
}

[[nodiscard]] Options parse_options(int argc, char** argv)
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            options.show_help = true;
            continue;
        }

        const std::string_view text = require_value(
            argc,
            argv,
            index,
            argument
        );
        if (argument == "--workload") {
            if (!valid_workload(text)) {
                throw std::invalid_argument(
                    "unknown workload: " + std::string(text)
                );
            }
            options.workload = text;
            continue;
        }

        const std::uint64_t value = parse_unsigned(argument, text);
        if (argument == "--operations") {
            if (value < 100 ||
                value > std::numeric_limits<std::size_t>::max()) {
                throw std::invalid_argument(
                    "--operations must be at least 100"
                );
            }
            options.operations = static_cast<std::size_t>(value);
        }
        else if (argument == "--dataset-records") {
            if (value == 0 ||
                value > std::numeric_limits<std::size_t>::max()) {
                throw std::invalid_argument(
                    "--dataset-records must be greater than zero"
                );
            }
            options.dataset_records = static_cast<std::size_t>(value);
        }
        else if (argument == "--maintenance-operations") {
            if (value < 100 ||
                value > std::numeric_limits<std::size_t>::max()) {
                throw std::invalid_argument(
                    "--maintenance-operations must be at least 100"
                );
            }
            options.maintenance_operations = static_cast<std::size_t>(value);
        }
        else if (argument == "--key-bytes") {
            if (value == 0 ||
                value > std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument(
                    "--key-bytes must be between 1 and UINT32_MAX"
                );
            }
            options.key_bytes = static_cast<std::uint32_t>(value);
        }
        else if (argument == "--value-bytes") {
            if (value > std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument(
                    "--value-bytes must be at most UINT32_MAX"
                );
            }
            options.value_bytes = static_cast<std::uint32_t>(value);
        }
        else if (argument == "--seed") {
            options.seed = value;
        }
        else if (argument == "--recovery-iterations") {
            if (value == 0 ||
                value > std::numeric_limits<std::size_t>::max()) {
                throw std::invalid_argument(
                    "--recovery-iterations must be greater than zero"
                );
            }
            options.recovery_iterations = static_cast<std::size_t>(value);
        }
        else {
            throw std::invalid_argument(
                "unknown option: " + std::string(argument)
            );
        }
    }
    return options;
}

void print_usage(std::ostream& output, std::string_view program)
{
    output
        << "Usage: " << program << " [options]\n\n"
        << "  --workload NAME          all, sequential_insert, random_insert,\n"
        << "                           random_read, missing_read, mixed_95_5,\n"
        << "                           wal_recovery, or flush_compaction\n"
        << "  --operations N           timed ops and recovery records (default: 10000)\n"
        << "  --dataset-records N      read/mixed fixture records (default: 200)\n"
        << "  --maintenance-operations N\n"
        << "                           flush/compaction writes (default: 200)\n"
        << "  --key-bytes N            fixed key size (default: 16)\n"
        << "  --value-bytes N          fixed value size (default: 256)\n"
        << "  --seed N                 deterministic random seed\n"
        << "  --recovery-iterations N  repeated Engine::open samples (default: 15)\n"
        << "  --help, -h               show this help\n";
}

[[nodiscard]] DBOptions base_options(
    const std::filesystem::path& path,
    bool create_if_missing = true
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

[[nodiscard]] std::vector<std::string> make_keys(
    const Options& options,
    std::size_t count,
    std::size_t first_index = 0
)
{
    std::vector<std::string> keys;
    keys.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        keys.push_back(kvdb::benchmark::make_key(
            options.key_bytes,
            first_index + index
        ));
    }
    return keys;
}

[[nodiscard]] std::string make_missing_key(
    const Options& options,
    std::size_t index
)
{
    std::string key = kvdb::benchmark::make_key(options.key_bytes, index);
    key.front() = static_cast<char>(0x7f);
    return key;
}

[[nodiscard]] std::uint64_t counter_delta(
    std::uint64_t before,
    std::uint64_t after,
    std::string_view name
)
{
    if (after < before) {
        throw std::runtime_error(
            std::string(name) + " counter moved backwards"
        );
    }
    return after - before;
}

template <class Operation>
[[nodiscard]] Metrics measure_operations(
    std::string workload,
    std::size_t dataset_records,
    std::size_t operation_count,
    Engine& engine,
    Operation&& operation
)
{
    std::vector<kvdb::benchmark::Timer::Duration> latencies;
    latencies.reserve(operation_count);

    const EngineStatistics statistics_before = engine.statistics();
    const std::uint64_t bytes_before =
        kvdb::benchmark::process_write_bytes();
    kvdb::benchmark::Timer wall_timer;

    for (std::size_t index = 0; index < operation_count; ++index) {
        kvdb::benchmark::Timer operation_timer;
        const Status status = operation(index);
        latencies.push_back(operation_timer.stop());
        if (!status.is_ok()) {
            throw status_error(workload + " operation failed", status);
        }
    }

    const kvdb::benchmark::Timer::Duration wall_time = wall_timer.stop();
    const std::uint64_t bytes_after =
        kvdb::benchmark::process_write_bytes();
    const EngineStatistics statistics_after = engine.statistics();

    const double seconds = std::chrono::duration<double>(wall_time).count();
    if (seconds <= 0.0) {
        throw std::runtime_error("workload wall time was not positive");
    }

    return Metrics{
        .workload = std::move(workload),
        .dataset_records = dataset_records,
        .operation_count = operation_count,
        .operations_per_second =
            static_cast<double>(operation_count) / seconds,
        .latency = kvdb::benchmark::summarize_timings(latencies),
        .bytes_written = counter_delta(
            bytes_before,
            bytes_after,
            "process write-byte"
        ),
        .flush_count = counter_delta(
            statistics_before.flush_count,
            statistics_after.flush_count,
            "flush"
        ),
        .compaction_count = counter_delta(
            statistics_before.compaction_count,
            statistics_after.compaction_count,
            "compaction"
        )
    };
}

[[nodiscard]] Metrics run_insert(
    const std::filesystem::path& root,
    const Options& options,
    bool random_order
)
{
    const std::string name = random_order
        ? std::string(random_insert)
        : std::string(sequential_insert);
    Engine engine(base_options(root / name));
    Status status = engine.open();
    if (!status.is_ok()) {
        throw status_error("could not open " + name + " database", status);
    }

    std::vector<std::string> keys = make_keys(options, options.operations);
    std::vector<std::size_t> order(options.operations);
    for (std::size_t index = 0; index < order.size(); ++index) {
        order[index] = index;
    }
    if (random_order) {
        std::mt19937_64 random(options.seed);
        std::shuffle(order.begin(), order.end(), random);
    }
    std::string value = kvdb::benchmark::make_value(options.value_bytes);

    Metrics metrics = measure_operations(
        name,
        options.operations,
        options.operations,
        engine,
        [&engine, &keys, &order, &value](std::size_t index) {
            return engine.put(keys[order[index]], value);
        }
    );

    Result<std::optional<std::string>> validation = engine.get(keys.back());
    if (!validation.is_ok() || !validation.value.has_value()) {
        throw std::runtime_error(name + " validation failed");
    }
    status = engine.close();
    if (!status.is_ok()) {
        throw status_error("could not close " + name + " database", status);
    }
    return metrics;
}

void populate_and_flush(
    Engine& engine,
    const Options& options,
    std::vector<std::string>& keys,
    std::string& value
)
{
    for (std::string& key : keys) {
        const Status status = engine.put(key, value);
        if (!status.is_ok()) {
            throw status_error("could not populate read fixture", status);
        }
    }
    const Status status = engine.flush();
    if (!status.is_ok()) {
        throw status_error("could not flush read fixture", status);
    }

    const std::size_t warmup_count = std::min<std::size_t>(keys.size(), 512);
    for (std::size_t index = 0; index < warmup_count; ++index) {
        Result<std::optional<std::string>> result = engine.get(keys[index]);
        if (!result.is_ok() || !result.value.has_value() ||
            result.value->size() != options.value_bytes) {
            throw std::runtime_error("read fixture warmup failed");
        }
    }
}

[[nodiscard]] Metrics run_random_reads(
    const std::filesystem::path& root,
    const Options& options,
    bool missing
)
{
    const std::string name = missing
        ? std::string(missing_read)
        : std::string(random_read);
    Engine engine(base_options(root / name));
    Status status = engine.open();
    if (!status.is_ok()) {
        throw status_error("could not open " + name + " database", status);
    }

    std::vector<std::string> keys = make_keys(
        options,
        options.dataset_records
    );
    std::string value = kvdb::benchmark::make_value(options.value_bytes);
    populate_and_flush(engine, options, keys, value);

    std::mt19937_64 random(options.seed + (missing ? 1u : 0u));
    std::vector<std::size_t> read_order(options.operations);
    std::uniform_int_distribution<std::size_t> distribution(
        0,
        keys.size() - 1
    );
    for (std::size_t& index : read_order) {
        index = distribution(random);
    }

    std::vector<std::string> missing_keys;
    if (missing) {
        missing_keys.reserve(options.operations);
        for (std::size_t index = 0; index < options.operations; ++index) {
            missing_keys.push_back(make_missing_key(options, index));
        }
        std::shuffle(missing_keys.begin(), missing_keys.end(), random);
    }

    Metrics metrics = measure_operations(
        name,
        keys.size(),
        options.operations,
        engine,
        [&engine, &keys, &missing_keys, &read_order, missing](
            std::size_t index
        ) {
            const std::string& key = missing
                ? missing_keys[index]
                : keys[read_order[index]];
            Result<std::optional<std::string>> result = engine.get(key);
            if (!result.is_ok()) {
                return result.status;
            }
            const bool found = result.value.has_value();
            if (found == missing) {
                return Status{
                    StatusCode::Corruption,
                    missing
                        ? "missing-key read unexpectedly found a value"
                        : "successful read unexpectedly missed"
                };
            }
            return Status::ok();
        }
    );

    status = engine.close();
    if (!status.is_ok()) {
        throw status_error("could not close " + name + " database", status);
    }
    return metrics;
}

[[nodiscard]] Metrics run_mixed(
    const std::filesystem::path& root,
    const Options& options
)
{
    Engine engine(base_options(root / std::string(mixed_95_5)));
    Status status = engine.open();
    if (!status.is_ok()) {
        throw status_error("could not open mixed database", status);
    }

    std::vector<std::string> keys = make_keys(
        options,
        options.dataset_records
    );
    std::string value = kvdb::benchmark::make_value(options.value_bytes);
    populate_and_flush(engine, options, keys, value);

    struct MixedOperation
    {
        bool write = false;
        std::size_t key_index = 0;
    };
    std::vector<MixedOperation> plan;
    plan.reserve(options.operations);
    std::vector<std::string> write_keys;
    write_keys.reserve(options.operations / 20);
    std::mt19937_64 random(options.seed + 2u);
    std::uniform_int_distribution<std::size_t> distribution(
        0,
        keys.size() - 1
    );
    for (std::size_t index = 0; index < options.operations; ++index) {
        const bool write = (index + 1) % 20 == 0;
        if (write) {
            write_keys.push_back(kvdb::benchmark::make_key(
                options.key_bytes,
                keys.size() + write_keys.size()
            ));
            plan.push_back(MixedOperation{
                .write = true,
                .key_index = write_keys.size() - 1
            });
        }
        else {
            plan.push_back(MixedOperation{
                .write = false,
                .key_index = distribution(random)
            });
        }
    }

    Metrics metrics = measure_operations(
        std::string(mixed_95_5),
        keys.size(),
        options.operations,
        engine,
        [&engine, &keys, &write_keys, &plan, &value](std::size_t index) {
            const MixedOperation& operation = plan[index];
            if (operation.write) {
                return engine.put(write_keys[operation.key_index], value);
            }
            Result<std::optional<std::string>> result =
                engine.get(keys[operation.key_index]);
            if (!result.is_ok()) {
                return result.status;
            }
            return result.value.has_value()
                ? Status::ok()
                : Status{
                    StatusCode::Corruption,
                    "mixed workload successful read missed"
                };
        }
    );

    status = engine.close();
    if (!status.is_ok()) {
        throw status_error("could not close mixed database", status);
    }
    return metrics;
}

[[nodiscard]] std::size_t maintenance_memtable_limit(
    const Options& options,
    std::size_t operation_count
)
{
    constexpr std::size_t estimated_node_overhead = 128;
    const std::uint64_t record_bytes =
        static_cast<std::uint64_t>(options.key_bytes) +
        static_cast<std::uint64_t>(options.value_bytes) +
        estimated_node_overhead;
    const std::uint64_t operation_count_u64 =
        static_cast<std::uint64_t>(operation_count);
    const std::uint64_t maximum =
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
    const std::uint64_t total = operation_count_u64 > maximum / record_bytes
        ? maximum
        : operation_count_u64 * record_bytes;
    const std::uint64_t limit = std::max(record_bytes, total / 8);
    return static_cast<std::size_t>(std::min(limit, maximum));
}

[[nodiscard]] Metrics run_flush_compaction(
    const std::filesystem::path& root,
    const Options& options
)
{
    DBOptions database_options = base_options(
        root / std::string(flush_compaction)
    );
    const std::uint64_t memtable_cap_u64 =
        (static_cast<std::uint64_t>(options.key_bytes) +
            static_cast<std::uint64_t>(options.value_bytes) + 128u) *
        128u;
    const std::size_t memtable_cap = static_cast<std::size_t>(
        std::min<std::uint64_t>(
            memtable_cap_u64,
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max()
            )
        )
    );
    database_options.memtable.size_limit =
        std::min<std::size_t>(
            maintenance_memtable_limit(
                options,
                options.maintenance_operations
            ),
            memtable_cap
        );
    database_options.compaction.enable_background_compaction = true;
    database_options.compaction.l0_file_count_trigger = 2;
    for (std::uint64_t& target :
        database_options.compaction.target_file_size_per_level) {
        target = 64u * 1024u;
    }

    Engine engine(std::move(database_options));
    Status status = engine.open();
    if (!status.is_ok()) {
        throw status_error("could not open maintenance database", status);
    }

    std::vector<std::string> keys = make_keys(
        options,
        options.maintenance_operations
    );
    std::string value = kvdb::benchmark::make_value(options.value_bytes);
    Metrics metrics = measure_operations(
        std::string(flush_compaction),
        options.maintenance_operations,
        options.maintenance_operations,
        engine,
        [&engine, &keys, &value](std::size_t index) {
            return engine.put(keys[index], value);
        }
    );

    if (metrics.flush_count == 0 || metrics.compaction_count == 0) {
        throw std::runtime_error(
            "maintenance workload did not trigger both flush and compaction"
        );
    }
    status = engine.close();
    if (!status.is_ok()) {
        throw status_error("could not close maintenance database", status);
    }
    return metrics;
}

void create_recovery_fixture(
    const std::filesystem::path& path,
    const Options& options
)
{
    Engine engine(base_options(path));
    Status status = engine.open();
    if (!status.is_ok()) {
        throw status_error("could not open recovery fixture", status);
    }
    std::vector<std::string> keys = make_keys(options, options.operations);
    std::string value = kvdb::benchmark::make_value(options.value_bytes);
    for (std::string& key : keys) {
        status = engine.put(key, value);
        if (!status.is_ok()) {
            throw status_error("could not populate recovery fixture", status);
        }
    }
    status = engine.close();
    if (!status.is_ok()) {
        throw status_error("could not close recovery fixture", status);
    }
}

[[nodiscard]] Metrics run_wal_recovery(
    const std::filesystem::path& root,
    const Options& options
)
{
    const std::filesystem::path fixture = root / "recovery_template";
    create_recovery_fixture(fixture, options);

    std::vector<kvdb::benchmark::Timer::Duration> latencies;
    latencies.reserve(options.recovery_iterations);
    kvdb::benchmark::Timer::Duration total_time{};
    std::uint64_t total_bytes_written = 0;

    for (std::size_t iteration = 0;
        iteration < options.recovery_iterations;
        ++iteration) {
        RunDirectory run_directory(
            fixture,
            root / ("recovery_run_" + std::to_string(iteration))
        );
        Engine engine(base_options(run_directory.path(), false));
        const std::uint64_t bytes_before =
            kvdb::benchmark::process_write_bytes();
        kvdb::benchmark::Timer timer;
        const Status status = engine.open();
        const kvdb::benchmark::Timer::Duration elapsed = timer.stop();
        const std::uint64_t bytes_after =
            kvdb::benchmark::process_write_bytes();
        if (!status.is_ok()) {
            throw status_error("WAL recovery failed", status);
        }

        latencies.push_back(elapsed);
        total_time += elapsed;
        const std::uint64_t iteration_bytes = counter_delta(
            bytes_before,
            bytes_after,
            "process write-byte"
        );
        if (iteration_bytes >
            std::numeric_limits<std::uint64_t>::max() -
                total_bytes_written) {
            throw std::runtime_error("recovery byte count overflowed");
        }
        total_bytes_written += iteration_bytes;

        const std::string final_key = kvdb::benchmark::make_key(
            options.key_bytes,
            options.operations - 1
        );
        Result<std::optional<std::string>> validation = engine.get(final_key);
        if (!validation.is_ok() || !validation.value.has_value()) {
            throw std::runtime_error("WAL recovery validation failed");
        }
        const Status close_status = engine.close();
        if (!close_status.is_ok()) {
            throw status_error("could not close recovered database", close_status);
        }
    }

    const double seconds = std::chrono::duration<double>(total_time).count();
    if (seconds <= 0.0) {
        throw std::runtime_error("WAL recovery time was not positive");
    }
    return Metrics{
        .workload = std::string(wal_recovery),
        .dataset_records = options.operations,
        .operation_count = options.recovery_iterations,
        .operations_per_second =
            static_cast<double>(options.recovery_iterations) / seconds,
        .latency = kvdb::benchmark::summarize_timings(latencies),
        .bytes_written = total_bytes_written,
        .flush_count = 0,
        .compaction_count = 0
    };
}

[[nodiscard]] bool selected(const Options& options, std::string_view name)
{
    return options.workload == all_workloads || options.workload == name;
}

void print_header(const Options& options)
{
    std::cout
        << "# KVDB operation workload benchmark\n"
        << "# operations=" << options.operations
        << ", dataset_records=" << options.dataset_records
        << ", maintenance_operations=" << options.maintenance_operations
        << ", key_bytes=" << options.key_bytes
        << ", value_bytes=" << options.value_bytes
        << ", seed=" << options.seed
        << ", recovery_iterations=" << options.recovery_iterations << '\n'
        << "# latencies include one Engine operation; fixture setup and "
           "validation are excluded\n"
        << "# bytes_written=OS-reported process write-transfer bytes in the "
           "measured region\n";
#ifdef NDEBUG
    std::cout << "# build=optimized (NDEBUG defined)\n";
#else
    std::cout << "# WARNING: debug build; use Release for comparable results\n";
#endif
    std::cout
        << "workload,dataset_records,operations,operations_per_second,"
           "p50_us,p95_us,p99_us,bytes_written,flush_count,compaction_count\n";
}

void print_metrics(const Metrics& metrics)
{
    std::cout << std::fixed << std::setprecision(3)
        << metrics.workload << ','
        << metrics.dataset_records << ','
        << metrics.operation_count << ','
        << metrics.operations_per_second << ','
        << metrics.latency.median_ms * 1'000.0 << ','
        << metrics.latency.p95_ms * 1'000.0 << ','
        << metrics.latency.p99_ms * 1'000.0 << ','
        << metrics.bytes_written << ','
        << metrics.flush_count << ','
        << metrics.compaction_count << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const Options options = parse_options(argc, argv);
        if (options.show_help) {
            print_usage(std::cout, argv[0]);
            return 0;
        }

        TemporaryDirectory temporary_directory;
        print_header(options);
        if (selected(options, sequential_insert)) {
            print_metrics(run_insert(
                temporary_directory.path(),
                options,
                false
            ));
        }
        if (selected(options, random_insert)) {
            print_metrics(run_insert(
                temporary_directory.path(),
                options,
                true
            ));
        }
        if (selected(options, random_read)) {
            print_metrics(run_random_reads(
                temporary_directory.path(),
                options,
                false
            ));
        }
        if (selected(options, missing_read)) {
            print_metrics(run_random_reads(
                temporary_directory.path(),
                options,
                true
            ));
        }
        if (selected(options, mixed_95_5)) {
            print_metrics(run_mixed(temporary_directory.path(), options));
        }
        if (selected(options, wal_recovery)) {
            print_metrics(run_wal_recovery(
                temporary_directory.path(),
                options
            ));
        }
        if (selected(options, flush_compaction)) {
            print_metrics(run_flush_compaction(
                temporary_directory.path(),
                options
            ));
        }
    }
    catch (const std::exception& exception) {
        std::cerr << "benchmark failed: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}
