#include "process_io.h"

#include <limits>
#include <stdexcept>

#ifdef _WIN32

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace kvdb::benchmark {

std::uint64_t process_write_bytes()
{
    IO_COUNTERS counters{};
    if (::GetProcessIoCounters(::GetCurrentProcess(), &counters) == 0) {
        throw std::runtime_error("GetProcessIoCounters failed");
    }
    return static_cast<std::uint64_t>(counters.WriteTransferCount);
}

} // namespace kvdb::benchmark

#elif defined(__linux__)

#include <fstream>
#include <string>

namespace kvdb::benchmark {

std::uint64_t process_write_bytes()
{
    std::ifstream input("/proc/self/io");
    std::string label;
    std::uint64_t value = 0;
    while (input >> label >> value) {
        // wchar is the total bytes supplied to write-like system calls. It is
        // the closest Linux equivalent to Windows WriteTransferCount.
        if (label == "wchar:") {
            return value;
        }
    }
    throw std::runtime_error("could not read wchar from /proc/self/io");
}

} // namespace kvdb::benchmark

#else

#include <sys/resource.h>

namespace kvdb::benchmark {

std::uint64_t process_write_bytes()
{
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_oublock < 0) {
        throw std::runtime_error("getrusage failed");
    }
    constexpr std::uint64_t assumed_block_bytes = 512;
    const auto blocks = static_cast<std::uint64_t>(usage.ru_oublock);
    if (blocks > std::numeric_limits<std::uint64_t>::max() /
        assumed_block_bytes) {
        throw std::runtime_error("process write-byte counter overflowed");
    }
    return blocks * assumed_block_bytes;
}

} // namespace kvdb::benchmark

#endif
