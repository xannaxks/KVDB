#pragma once

#include <chrono>
#include <span>

namespace kvdb::benchmark {

/** Monotonic wall-clock timer with no I/O in the measured path. */
class Timer final
{
public:
    using Clock = std::chrono::steady_clock;
    using Duration = std::chrono::nanoseconds;

    Timer() noexcept;

    void reset() noexcept;
    [[nodiscard]] Duration stop() noexcept;
    [[nodiscard]] Duration elapsed() const noexcept;

private:
    Clock::time_point start_{};
    Clock::time_point end_{};
    bool running_ = false;
};

struct TimingSummary
{
    double minimum_ms = 0.0;
    double median_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double maximum_ms = 0.0;
    double mean_ms = 0.0;
    double standard_deviation_ms = 0.0;
};

/** Summarizes a non-empty set of independent benchmark samples. */
[[nodiscard]] TimingSummary summarize_timings(
    std::span<const Timer::Duration> samples
);

} // namespace kvdb::benchmark
