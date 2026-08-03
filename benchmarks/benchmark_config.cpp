#include "benchmark_config.h"

#include <charconv>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <system_error>

namespace kvdb::benchmark {
namespace {

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

} // namespace

Options parse_options(int argc, char** argv)
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
        const std::uint64_t value = parse_unsigned(argument, text);

        if (argument == "--warmup") {
            if (value > std::numeric_limits<std::size_t>::max()) {
                throw std::invalid_argument("--warmup is too large");
            }
            options.warmup_iterations = static_cast<std::size_t>(value);
        }
        else if (argument == "--iterations") {
            if (value == 0 ||
                value > std::numeric_limits<std::size_t>::max()) {
                throw std::invalid_argument(
                    "--iterations must be greater than zero"
                );
            }
            options.measured_iterations = static_cast<std::size_t>(value);
        }
        else if (argument == "--records") {
            if (value == 0 ||
                value > std::numeric_limits<std::size_t>::max()) {
                throw std::invalid_argument(
                    "--records must be greater than zero"
                );
            }
            options.records = static_cast<std::size_t>(value);
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
        else {
            throw std::invalid_argument(
                "unknown option: " + std::string(argument)
            );
        }
    }
    return options;
}

std::vector<Scenario> make_scenarios(const Options& options)
{
    const bool custom = options.records.has_value() ||
        options.key_bytes.has_value() ||
        options.value_bytes.has_value();
    if (custom) {
        return {
            Scenario{
                .name = "custom",
                .records = options.records.value_or(10'000),
                .key_bytes = options.key_bytes.value_or(16),
                .value_bytes = options.value_bytes.value_or(1'024)
            }
        };
    }

    return {
        Scenario{
            .name = "small",
            .records = 50'000,
            .key_bytes = 16,
            .value_bytes = 100
        },
        Scenario{
            .name = "medium",
            .records = 10'000,
            .key_bytes = 16,
            .value_bytes = 1'024
        },
        Scenario{
            .name = "fragmented",
            .records = 1'000,
            .key_bytes = 16,
            .value_bytes = 16'384
        }
    };
}

void print_usage(std::ostream& output, std::string_view program_name)
{
    output
        << "Usage: " << program_name << " [options]\n"
        << "\n"
        << "With no workload options, runs small, medium, and fragmented "
           "record scenarios.\n"
        << "Specifying any workload option runs one custom scenario.\n"
        << "\n"
        << "  --records N       logical records (custom default: 10000)\n"
        << "  --key-bytes N     bytes per key (custom default: 16)\n"
        << "  --value-bytes N   bytes per value (custom default: 1024)\n"
        << "  --warmup N        unreported warm-up runs (default: 2)\n"
        << "  --iterations N    measured runs (default: 7)\n"
        << "  --help, -h        show this help\n";
}

std::string make_key(std::uint32_t size, std::size_t record_index)
{
    static constexpr std::string_view digits =
        "0123456789abcdefghijklmnopqrstuvwxyz";
    std::string key(size, 'k');
    std::size_t value = record_index;
    for (std::size_t position = key.size(); position > 0; --position) {
        key[position - 1] = digits[value % digits.size()];
        value /= digits.size();
        if (value == 0) {
            break;
        }
    }
    return key;
}

std::string make_value(std::uint32_t size)
{
    std::string value(size, '\0');
    for (std::size_t index = 0; index < value.size(); ++index) {
        value[index] = static_cast<char>('!' + (index % 90));
    }
    return value;
}

} // namespace kvdb::benchmark
