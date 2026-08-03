# Engine

`Engine` is the concrete implementation of the public [`KVDB`](../../include/kvdb.h)
interface. It owns the runtime state of one database and coordinates recovery,
sequence assignment, WAL durability, MemTable visibility, SSTable publication,
level metadata, and compaction.

The engine is an orchestrator rather than a storage format. The formats and
data structures it coordinates are documented separately:

- [WAL](./wal.md)
- [MemTable](./mem_table.md)
- [SSTable](./sstable.md)
- [Manifest](./manifest.md)
- [Level manager](./level-manager.md)
- [Arena](./arena.md)

## Ownership

An open engine owns:

```cpp
DBOptions options_;

std::unique_ptr<Manifest> manifest_;
std::unique_ptr<LevelManager> level_manager_;
std::unique_ptr<WAL> wal_;
std::unique_ptr<MemTable> mem_table_;
std::unique_ptr<CompactionScheduler> compaction_scheduler_;
std::unique_ptr<SSTableManager> sstable_manager_;
std::unique_ptr<Arena> arena_;

std::uint64_t next_sequence_ = 1;
std::uint32_t current_wal_id_ = 1;
```

The manifest is the durable authority for `next_table_id`,
`next_sequence_number`, `current_wal_id`, and active table metadata. The level
manager is the in-memory projection of the manifest's table edits. The engine
arena owns bytes referenced by MemTables and persistent metadata structures.

`SSTableManager` owns an id-keyed cache of loaded, immutable `SSTable` objects.
Each loaded table keeps its readable file and lazy `DataSectionView` alive.

## State model

The engine tracks two lifecycle flags:

```text
constructed: opened = false, closed = false
open:        opened = true,  closed = false
closed:      opened = false, closed = true
```

`open()` is idempotent while already open. `close()` is idempotent after close,
but a closed instance cannot be reopened. Public data operations call
`ensure_open()` and report either `FailedPrecondition` or `UseAfterClose` when
the state is invalid.

The public factory `KVDB::open(options)` constructs an engine, runs recovery,
and returns the pointer only after `Engine::open()` succeeds. Allocation and
ordinary exceptions are converted into `Status` values at this boundary.

## Open sequence

Opening is serialized by the engine mutex and proceeds in this order:

```text
validate options
prepare database and SSTable directories
allocate runtime components
load/create manifest
load/create current WAL
mark engine open
```

### Directory preparation

`prepare_dirs()` enforces `create_if_missing` and `error_if_exists`, requires
`db_path` to be a directory, creates `<db_path>/sstables`, and selects:

```text
manifest: <db_path>/MANIFEST
WAL:      <db_path>/wal-NNNNNNNNN.log
SSTable:  <db_path>/sstables/table-NNNNNNNNN.sst
```

### Manifest recovery

If `MANIFEST` exists and is non-empty, `Manifest::load()` replays its valid
prefix into the level manager. `prepare_for_append()` handles a recoverable torn
tail, after which an append writer is attached. A missing or empty manifest is
created with a durable header.

Manifest replay is transactional with respect to the supplied level manager:
failed recovery does not expose a partially rebuilt catalog.

### WAL recovery

The manifest-selected WAL id and next sequence initialize engine counters. If
the selected file does not exist, a fresh WAL is created. Otherwise the engine:

```text
recover complete WAL records into the arena
reject durable-prefix corruption
apply recovered records to the MemTable
advance next_sequence beyond every recovered record
create WAL generation current_id + 1
rewrite all recovered records into the replacement WAL
sync replacement WAL
commit replacement id and next sequence to the manifest
switch to the replacement and delete the old file
```

An incomplete final fragment is a recoverable torn tail. Checksum errors,
invalid fragment sequences, or invalid formats in the durable prefix cause
`Engine::open()` to fail.

## Mutation path

`put()` and `remove()` take the engine mutex and delegate to `put_impl()`.
`remove()` creates a tombstone with an empty value.

```text
put_impl(key, value, type):
    ensure engine is open
    validate type and sequence capacity
    checkpoint engine arena
    copy key and value into arena
    record = InternalRecord(key, value, type, next_sequence)
    append record to WAL
    if sync_on_write:
        sync WAL
    apply record to mutable MemTable
    increment next_sequence
    maybe flush
```

The arena checkpoint allows failed allocations or a failed WAL append to
release the new bytes. The visibility invariant is:

```text
WAL append succeeds before MemTable insertion
```

With `DBOptions::wal.sync_on_write = true`, the WAL is also synchronized before
the record becomes visible. With the default `false`, every write avoids a
per-operation sync; later flush, rotation, or close provides a synchronization
boundary.

After insertion, `maybe_flush_unlocked()` compares the active MemTable's
approximate memory usage and current WAL offset to their configured limits.
Crossing either limit performs a synchronous flush before the mutation call
returns.

## Point lookup

`get()` validates the key, then searches from newest storage to oldest:

```text
MemTable generations
    |
    | miss
    v
L0 candidate tables, newest table id first
    |
    v
at most one candidate from each L1+ level
```

`MemTable::get()` already chooses the highest sequence number across all memory
generations. A returned tombstone becomes an empty optional and stops the
search.

For on-disk lookup, the engine asks `LevelManager` for tables whose inclusive
range contains the key. It opens each table through `SSTableManager`, calls
`SSTable::get()`, and continues only for an absent key. A found tombstone again
stops lookup.

The table lookup currently uses the index and lazy data-block view. Although
the SSTable contains a validated Bloom section, `SSTable::get()` does not yet
consult it.

A short-lived read arena owns record bytes loaded from an SSTable. Before that
arena is destroyed, `api_value()` converts a `Put` value to an owning
`std::string` or a tombstone to `std::nullopt`.

## Flush protocol

`flush_unlocked()` freezes a non-empty active MemTable and drains every queued
immutable generation oldest-first. An empty active MemTable is a successful
no-op.

For each immutable generation, `flush_oldest_immutable()` performs:

```text
1. Pin oldest generation and remember generation_id.
2. Reserve manifest.next_table_id as a 32-bit SSTable id.
3. Build a sorted SSTable at a temporary path.
4. Write, sync, rename, and directory-sync the SSTable.
5. Derive L0 TableMeta in the engine arena.
6. Prepare a VersionEdit that adds the table and advances counters.
7. If this is the final queued immutable, create a replacement WAL and add its
   id to the edit.
8. Commit the VersionEdit to the manifest and level manager.
9. Retire exactly the pinned immutable generation_id.
10. If rotating, close the old WAL, install the replacement, and delete the
    old WAL file.
```

The immutable stays readable until step 9. The generation id prevents flush
code from retiring a different queue entry if state changes unexpectedly.

The new SSTable is published before its metadata edit. The old memory and WAL
are retired only after the manifest commit, preserving recovery coverage across
each transition.

After all immutable generations are drained, the engine checks compaction
pressure.

## Automatic compaction

`maybe_compact_unlocked()` runs only when
`options.compaction.enable_background_compaction` is true. Despite the option
name, the current implementation creates no background thread: it repeatedly
picks and runs compactions inline while holding the engine mutex.

The scheduler prioritizes:

1. L0 when `l0_file_count_trigger` is reached.
2. The first compactable L1+ level whose aggregate bytes exceed its limit.

For L0, every source table is selected. For a higher level, the largest source
table is selected. The plan includes all overlapping tables in the adjacent
destination level and uses that destination's target file size.

`run_compaction()` delegates the merge to `CompactionJob`. If the job returns a
`VersionEdit`, the engine gathers the input paths, commits the edit, increments
statistics, and only then deletes obsolete input files.

## Manual range compaction

`compact_range(begin, end)` first calls `flush_unlocked()` so the requested
state is represented in SSTables. It rejects a reversed range, then visits each
source level that has a destination.

For each level it repeatedly expands the interval with overlapping source and
destination tables until the selected set and bounds stop changing. This
closure is necessary to avoid leaving overlaps in an L1+ destination.

The implementation's overlap checks are inclusive at both ends. Empty source
selections are skipped; non-empty plans are validated and committed using the
normal compaction protocol.

## Close behavior

`close()` currently:

```text
sync WAL
sync manifest
close WAL
mark engine closed
```

It does not force a MemTable flush. This is safe only through WAL recovery:
unflushed records remain in the synchronized WAL and are replayed on the next
open. Calling `flush()` before `close()` is still useful when the caller wants
the current state materialized as SSTables.

The destructor calls `close()` but cannot report an error, so applications that
need close-error handling should call it explicitly.

## Concurrency model

Every public operation—`open`, `put`, `get`, `remove`, `flush`,
`compact_range`, `close`, and `statistics`—uses the same `std::mutex`. This
provides a simple thread-safe API and keeps these operations atomic with respect
to one another:

- sequence assignment and WAL order;
- WAL append and MemTable visibility;
- table-id allocation and manifest commit;
- compaction plan selection and publication.

The cost is that reads cannot overlap other reads or maintenance through the
engine API. `MemTable` itself uses a shared mutex, but the outer engine mutex is
the effective concurrency boundary today.

## Statistics

`EngineStatistics` exposes monotonic counters:

```cpp
struct EngineStatistics
{
    std::uint64_t flush_count = 0;
    std::uint64_t compaction_count = 0;
};
```

A flush is counted only after an immutable generation is published and retired.
A compaction is counted only after its manifest edit commits. Empty operations
do not increment counters, and counters saturate rather than overflow.

## Configuration used by the engine

The active orchestration policies include:

| Option | Effect |
| --- | --- |
| `db_path`, `create_if_missing`, `error_if_exists` | Directory handling |
| `arena.page_size`, `arena.large_threshold` | Engine and read-arena allocation |
| `memtable.size_limit` | Automatic flush threshold |
| `wal.file_size_limit` | Automatic flush/WAL-rotation threshold |
| `wal.sync_on_write` | Per-mutation WAL sync |
| `compaction.max_levels` | LevelManager size |
| `compaction.enable_background_compaction` | Enables inline automatic compaction checks |
| L0/level byte triggers | Scheduler pressure thresholds |
| target file sizes | Compaction output rolling |

Some validated options are reserved for future integration. In particular,
`immutable_tables_limit`, manifest rewrite size, SSTable lazy-loading policy,
and configurable format block/Bloom values do not currently change engine
behavior.

## Failure and publication rules

The engine relies on the following ordering rules:

```text
mutation:    WAL -> optional sync -> MemTable
flush:       publish SSTable -> commit manifest -> retire generation/WAL
compaction:  publish outputs -> commit manifest -> delete inputs
startup:     replay manifest -> replay selected WAL -> publish open engine
```

Failures before a manifest commit can leave unreferenced output files, but do
not install them in the active level state. A failed manifest append/sync
poisons the manifest writer because its tail may be uncertain; recovery is
required before further metadata commits.

## Source

- Interface: [`include/engine.h`](../../include/engine.h)
- Implementation: [`src/engine.cpp`](../../src/engine.cpp)
- Public factory: [`src/kvdb.cpp`](../../src/kvdb.cpp)
- Integration tests: [`tests/engine_test.cpp`](../../tests/engine_test.cpp)
