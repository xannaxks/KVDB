# KVDB benchmarks

The WAL recovery benchmarks answer two different questions:

- `kvdb_wal_benchmark`: how fast can the WAL loader open, read, checksum,
  decode, and reconstruct records into an `Arena`?
- `kvdb_engine_benchmark`: how long until `Engine::open()` finishes recovery
  and the database is ready? This includes manifest loading, `WAL::recover()`,
  MemTable replay, replacement-WAL writing and syncing, a manifest commit, and
  deletion of the old WAL.
- `kvdb_workload_benchmark`: operation throughput and latency distributions for
  inserts, reads, a mixed workload, recovery, and forced maintenance. It also
  reports process write-transfer bytes and exact Engine flush/compaction counts.

The second number should be larger. It is the user-visible recovery latency;
the first isolates the WAL parser and is more useful when optimizing WAL code.

## Build correctly

Always measure an optimized build. Debug builds, sanitizers, coverage, logging,
and a debugger can change results by multiples.

For Visual Studio or another multi-config generator, run these commands from a
Visual Studio Developer PowerShell/Command Prompt so `cl.exe` and the Windows
SDK are on `PATH`:

```powershell
cmake -S . -B build-bench -DKVDB_ENABLE_BENCHMARKS=ON -DKVDB_ENABLE_TESTS=OFF
cmake --build build-bench --config Release --target kvdb_wal_benchmark kvdb_engine_benchmark kvdb_workload_benchmark
./build-bench/Release/kvdb_wal_benchmark.exe
./build-bench/Release/kvdb_engine_benchmark.exe
./build-bench/Release/kvdb_workload_benchmark.exe
```

For Ninja, Makefiles, or another single-config generator:

```sh
cmake -S . -B build-bench \
  -DCMAKE_BUILD_TYPE=Release \
  -DKVDB_ENABLE_BENCHMARKS=ON \
  -DKVDB_ENABLE_TESTS=OFF
cmake --build build-bench --parallel \
  --target kvdb_wal_benchmark kvdb_engine_benchmark kvdb_workload_benchmark
./build-bench/kvdb_wal_benchmark
./build-bench/kvdb_engine_benchmark
./build-bench/kvdb_workload_benchmark
```

The benchmark support code is built only when `KVDB_ENABLE_BENCHMARKS=ON`; the
production engine does not depend on the timer.

## Operation workloads

`kvdb_workload_benchmark` runs all of these workloads by default:

| Workload | Default measured work | Setup outside measurement |
|---|---:|---|
| `sequential_insert` | 10,000 ordered puts | empty database |
| `random_insert` | 10,000 deterministically shuffled puts | empty database |
| `random_read` | 10,000 successful random gets | 200 records flushed into one warm SSTable |
| `missing_read` | 10,000 random absent-key gets | same warm 200-record SSTable |
| `mixed_95_5` | exactly 95% successful gets, 5% puts | same warm 200-record SSTable |
| `wal_recovery` | 15 fresh `Engine::open()` calls | copied database with 10,000 WAL records |
| `flush_compaction` | 200 puts | small MemTable and L0 trigger of 2 |

The maintenance workload uses conservative current-format table sizes and
fails unless both a flush and a compaction are observed. The read fixture and
maintenance sizes are deliberately separate from the main operation count so
they can be changed without changing the number of latency samples.

The defaults also stay below two known current format boundaries: a large
single SSTable can exceed the version-1 single-block index, and enough repeated
manifest edits can expose a payload-size mismatch. Custom sizes are never
silently reduced; if they cross a current engine boundary, the benchmark exits
with the engine error instead of publishing misleading results.

Run one workload or customize the sizes:

```powershell
./build-bench/Release/kvdb_workload_benchmark.exe `
  --workload mixed_95_5 `
  --operations 50000 `
  --dataset-records 200 `
  --key-bytes 16 `
  --value-bytes 256
```

Use `--help` to list workload names and all options. The fixed random seed makes
insertion order and read selection reproducible; change it with `--seed`.

## Run workloads

With no arguments, each executable runs three workloads:

| Scenario | Records | Key | Value | Why |
|---|---:|---:|---:|---|
| `small` | 50,000 | 16 B | 100 B | record-processing overhead |
| `medium` | 10,000 | 16 B | 1 KiB | ordinary throughput |
| `fragmented` | 1,000 | 16 B | 16 KiB | multi-block fragment assembly |

Run a custom workload by specifying any workload option:

```powershell
./build-bench/Release/kvdb_wal_benchmark.exe `
  --records 100000 --key-bytes 24 --value-bytes 512 `
  --warmup 3 --iterations 15
```

Use `--help` for all options. Output is CSV with comment lines beginning with
`#`, so it can be redirected and compared across commits:

```powershell
./build-bench/Release/kvdb_wal_benchmark.exe > wal-results.csv
```

## Metrics

Each reported row is calculated from independent timed runs after the requested
warmups.

| Metric | Meaning | Best use |
|---|---|---|
| `median_ms` | p50 wall-clock recovery latency | primary comparison metric |
| `p95_ms` | interpolated 95th percentile | run-to-run tail variability |
| `p99_ms` | interpolated 99th percentile | severe tail latency |
| `min_ms`, `max_ms` | fastest and slowest samples | spot noise and outliers |
| `mean_ms`, `stddev_ms` | arithmetic mean and population standard deviation | stability check |
| `records_per_second` | records divided by median latency | record-heavy workloads |
| `mib_per_second` | physical WAL MiB divided by median latency | byte-heavy workloads |
| `ns_per_record` | median latency divided by record count | normalized CPU/work cost |
| `arena_used_mib` | bytes occupied by recovered key/value allocations | loader memory payload |
| `arena_reserved_mib` | total pages reserved by the recovery Arena | allocator footprint |

The workload benchmark reports the requested operation metrics in one CSV:

| Metric | Definition |
|---|---|
| `operations_per_second` | completed operations divided by total measured wall time; includes flush/compaction stalls |
| `p50_us`, `p95_us`, `p99_us` | percentiles across individual Engine call durations |
| `bytes_written` | OS-reported process write-transfer bytes between snapshots around the measured region |
| `flush_count` | immutable MemTable generations successfully published and retired |
| `compaction_count` | compaction jobs whose manifest edit was successfully committed |

On Windows, write bytes use `GetProcessIoCounters().WriteTransferCount`; on
Linux they use `/proc/self/io`'s `wchar`. Other POSIX systems use output blocks
times 512 as a documented fallback. This is cumulative write traffic, not final
database size, so it continues to count SSTables later deleted by compaction.

Throughput columns use the median latency, not the mean. Always compare the raw
latency distribution as well: a higher records/s value can simply mean the test
used smaller records.

## What is and is not timed

Fixture generation, fixture copying, result validation, cleanup, and printing
are outside the timed region. `Timer` uses `std::chrono::steady_clock`, measures
the duration directly, and performs no destructor I/O.

For operation workloads, key generation and deterministic operation planning
are also outside measurement. Each Engine call is sampled for latency, while a
separate wall timer surrounds the whole loop for throughput. Read fixtures are
warmed before measurement. WAL-recovery fixture copying is outside each timed
`Engine::open()` call.

Both executables measure warm-cache recovery. The isolated benchmark creates
and warms its fixture before measurement. The engine benchmark copies a fresh
database directory before each timed run, which normally makes its input
resident in the operating-system page cache. This is deliberate and
repeatable; it measures implementation cost with storage-cache misses mostly
removed.

Cold-cache recovery is a separate experiment. It requires an OS-specific cache
eviction procedure or a machine restart, generally needs elevated privileges,
and should report the storage device and filesystem. Do not label the first run
"cold" merely because it is first: creating or copying the fixture has already
warmed it.

## Benchmark results on test machine

These results were produced by an optimized build with `NDEBUG` defined.

### Characteristics
CPU: AMD Ryzen 5 5600H, 6 cores, 12 threads, 3.3 GHz base, 4.2 GHz boost 

Memory: 16 GB DDR4, 3200 MHz

Storage: Micron 2300 NVMe SSD, 512 GB, PCIe Gen3 x4 (protocol NVMe 1.3)

OS: Windows 11 Pro , 64-bit, version 25H2, build 26200.8875, NTFS

Compiler: MSVC 19.51.36252 for x86

Build: Release, -O3 -DNDEBUG

KVDB: v0.1.0, commit acabae2


### Isolated WAL recovery

Source: `results/wal_benchmark.csv`. The benchmark used two warmup iterations
and seven measured iterations with a warm cache.

| Scenario | Records | Key bytes | Value bytes | WAL MiB | Min ms | Median ms | p95 ms | p99 ms | Max ms | Mean ms | Stddev ms | Records/s | MiB/s | ns/record | Arena used MiB | Arena reserved MiB |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| small | 50,000 | 16 | 100 | 6.976 | 895.219 | 943.728 | 1,101.378 | 1,142.818 | 1,153.177 | 966.897 | 80.782 | 52,981.379 | 7.392 | 18,874.556 | 5.531 | 5.938 |
| medium | 10,000 | 16 | 1,024 | 10.259 | 227.210 | 234.744 | 258.035 | 258.729 | 258.903 | 240.521 | 11.381 | 42,599.670 | 43.702 | 23,474.360 | 9.918 | 9.938 |
| fragmented | 1,000 | 16 | 16,384 | 15.753 | 106.873 | 116.347 | 122.244 | 123.946 | 124.371 | 114.861 | 5.674 | 8,595.001 | 135.401 | 116,346.700 | 15.640 | 15.688 |

### End-to-end engine recovery

Source: `results/engine_benchmark.csv`. The benchmark used two warmup
iterations and seven measured iterations. Each timed run used a fresh fixture
copy with a warm cache; fixture copying was not timed.

| Scenario | Records | Key bytes | Value bytes | WAL MiB | Min ms | Median ms | p95 ms | p99 ms | Max ms | Mean ms | Stddev ms | Records/s | MiB/s | ns/record |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| small | 50,000 | 16 | 100 | 6.976 | 3,682.971 | 3,739.077 | 3,767.076 | 3,772.480 | 3,773.831 | 3,731.673 | 28.737 | 13,372.283 | 1.866 | 74,781.546 |
| medium | 10,000 | 16 | 1,024 | 10.259 | 976.588 | 993.639 | 1,002.559 | 1,004.389 | 1,004.847 | 992.608 | 8.206 | 10,064.014 | 10.325 | 99,363.930 |
| fragmented | 1,000 | 16 | 16,384 | 15.753 | 455.656 | 474.313 | 479.965 | 480.262 | 480.336 | 471.709 | 8.923 | 2,108.314 | 33.213 | 474,312.600 |

### Operation workloads

Source: `results/workload_benchmark.csv`. The benchmark used 10,000 operations,
200 dataset records, 200 maintenance operations, 16-byte keys, 256-byte values,
seed `1263944770`, and 15 recovery iterations.

| Workload | Dataset records | Operations | Operations/s | p50 us | p95 us | p99 us | Bytes written | Flushes | Compactions |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sequential_insert | 10,000 | 10,000 | 7,756.458 | 112.400 | 227.110 | 336.201 | 3,034,174 | 0 | 0 |
| random_insert | 10,000 | 10,000 | 6,471.494 | 147.900 | 260.700 | 387.201 | 3,034,174 | 0 | 0 |
| random_read | 200 | 10,000 | 39,667.397 | 19.000 | 58.605 | 92.101 | 0 | 0 | 0 |
| missing_read | 200 | 10,000 | 1,420,293.149 | 0.700 | 0.800 | 1.100 | 0 | 0 | 0 |
| mixed_95_5 | 200 | 10,000 | 54,914.731 | 13.400 | 71.610 | 85.803 | 151,814 | 0 | 0 |
| wal_recovery | 10,000 | 15 | 1.113 | 820,898.100 | 1,236,071.950 | 1,266,243.910 | 45,513,750 | 0 | 0 |
| flush_compaction | 200 | 200 | 1,426.900 | 66.800 | 266.475 | 25,348.825 | 404,101 | 6 | 3 |

## Getting useful numbers

1. Use the same Release compiler, flags, machine, filesystem, and power mode for
   both commits.
2. Close high-I/O applications and run enough iterations that the median is
   stable. Seven is a quick default; 15-30 is better for reports.
3. Record both loader-only and end-to-end results. If only the engine result
   regresses, investigate MemTable replay, WAL rewrite/sync, or manifest work.
4. Test several record sizes. WAL recovery includes fixed per-record work and
   per-byte work, and large records also cross fragment/block boundaries.
5. Keep correctness checks. A benchmark that silently recovers fewer records
   can look faster while being wrong.

The old timer placed around `WAL::recover()` in `Engine::open_wal()` measured
only the loader portion, printed on every database open, and made the production
engine depend on benchmark code. Keeping measurement in dedicated executables
makes repetitions, workload control, result validation, and Release-only builds
explicit.
