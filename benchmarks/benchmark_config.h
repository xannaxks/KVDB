#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kvdb::benchmark {

struct Scenario
{
    std::string name;
    std::size_t records = 0;
    std::uint32_t key_bytes = 0;
    std::uint32_t value_bytes = 0;
};

struct Options
{
    std::size_t warmup_iterations = 2;
    std::size_t measured_iterations = 7;
    std::optional<std::size_t> records;
    std::optional<std::uint32_t> key_bytes;
    std::optional<std::uint32_t> value_bytes;
    bool show_help = false;
};

[[nodiscard]] Options parse_options(int argc, char** argv);
[[nodiscard]] std::vector<Scenario> make_scenarios(const Options& options);
void print_usage(std::ostream& output, std::string_view program_name);

/** Deterministic fixed-size data keeps fixture generation reproducible. */
[[nodiscard]] std::string make_key(
    std::uint32_t size,
    std::size_t record_index
);
[[nodiscard]] std::string make_value(std::uint32_t size);

} // namespace kvdb::benchmark
