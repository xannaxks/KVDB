#include "timer.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace kvdb::benchmark {

Timer::Timer() noexcept
{
    reset();
}

void Timer::reset() noexcept
{
    start_ = Clock::now();
    end_ = start_;
    running_ = true;
}

Timer::Duration Timer::stop() noexcept
{
    if (running_) {
        end_ = Clock::now();
        running_ = false;
    }
    return elapsed();
}

Timer::Duration Timer::elapsed() const noexcept
{
    const Clock::time_point finish = running_ ? Clock::now() : end_;
    return std::chrono::duration_cast<Duration>(finish - start_);
}

TimingSummary summarize_timings(
    std::span<const Timer::Duration> samples
)
{
    if (samples.empty()) {
        throw std::invalid_argument("cannot summarize zero timing samples");
    }

    std::vector<double> milliseconds;
    milliseconds.reserve(samples.size());
    for (const Timer::Duration sample : samples) {
        milliseconds.push_back(
            std::chrono::duration<double, std::milli>(sample).count()
        );
    }
    std::sort(milliseconds.begin(), milliseconds.end());

    const auto percentile = [&milliseconds](double fraction) {
        const double index = fraction *
            static_cast<double>(milliseconds.size() - 1);
        const auto lower = static_cast<std::size_t>(std::floor(index));
        const auto upper = static_cast<std::size_t>(std::ceil(index));
        const double weight = index - static_cast<double>(lower);
        return milliseconds[lower] * (1.0 - weight) +
            milliseconds[upper] * weight;
    };

    double sum = 0.0;
    for (const double sample : milliseconds) {
        sum += sample;
    }
    const double mean = sum / static_cast<double>(milliseconds.size());

    double squared_difference_sum = 0.0;
    for (const double sample : milliseconds) {
        const double difference = sample - mean;
        squared_difference_sum += difference * difference;
    }

    return TimingSummary{
        .minimum_ms = milliseconds.front(),
        .median_ms = percentile(0.50),
        .p95_ms = percentile(0.95),
        .p99_ms = percentile(0.99),
        .maximum_ms = milliseconds.back(),
        .mean_ms = mean,
        .standard_deviation_ms = std::sqrt(
            squared_difference_sum /
            static_cast<double>(milliseconds.size())
        )
    };
}

} // namespace kvdb::benchmark
