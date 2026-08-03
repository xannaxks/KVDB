#pragma once

#include <cstdint>

namespace kvdb::benchmark {

/**
 * Returns the process's cumulative OS-reported write-transfer bytes.
 *
 * Take two snapshots around a measured region and subtract them. Fixture
 * creation, validation, and printing must remain outside those snapshots.
 */
[[nodiscard]] std::uint64_t process_write_bytes();

} // namespace kvdb::benchmark
