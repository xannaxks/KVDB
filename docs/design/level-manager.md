# Level Manager

`LevelManager` is KVDB's in-memory catalog of active SSTables. It groups
[`TableMeta`](../../include/table_meta.h) entries by LSM level and enforces the
different ordering rules required by level zero and higher levels.

It does not open, write, or delete SSTable files. It manages metadata only.
Durable changes are recorded by the [Manifest](./manifest.md), which stages and
publishes edits against a `LevelManager`.

## Data model

The manager stores one vector per configured level:

```cpp
std::vector<std::vector<TableMeta>> levels_;
```

The constructor requires at least one level. The engine normally constructs it
with `DBOptions::compaction.max_levels` (seven by default).

Each `TableMeta` contains:

- database-wide table id and level;
- SSTable path and file size;
- minimum and maximum sequence numbers;
- record, tombstone, block, and data-byte counts;
- inclusive smallest and largest user keys.

The key bytes referenced by `TableMeta::smallest_key` and `largest_key` are not
owned by the manager. In normal engine operation they live in the engine arena.

## Level invariants

### Level zero

L0 tables may overlap because each MemTable flush creates a new independent
SSTable.

```text
L0, newest first:

table 9:       [c-------------m]
table 7:  [a--------f]
table 5:             [h-------------z]
```

The L0 vector is ordered by table id descending. KVDB relies on table ids being
monotonic and never reused, so this is also newest-first order. Point lookup can
therefore stop at the first value or tombstone found among matching L0 tables.

### Level one and above

Every L1+ vector is sorted by `smallest_key` and contains disjoint inclusive
ranges:

```text
L2: [a---f] [g---m] [n---t] [u---z]
```

Because endpoints are inclusive, two ranges sharing the same endpoint overlap
and cannot coexist in an L1+ level. These invariants let point lookup use binary
search and return at most one table per higher level.

### Global invariants

Across all levels:

- table id zero is reserved;
- a table id may appear only once in the entire manager;
- a table's level must exist;
- `smallest_key <= largest_key`;
- L0 ordering and L1+ range rules must remain intact.

`LevelManager::add_table()` checks these catalog-level invariants. Intrinsic
metadata validation, such as path and encoded-size constraints, belongs to
`TableMeta::validate()` and the manifest layer.

## Adding a table

`add_table(TableMeta&& table)` first rejects a reserved id, invalid level,
inverted range, or database-wide duplicate id.

For L0 it finds the descending-id insertion point:

```text
insert L0 table:
    position = first existing table with table_id <= new.table_id
    insert at position
```

For L1+ it finds the ascending-key insertion point and compares only the
immediate previous and next ranges. Since the vector is already sorted and
non-overlapping, checking these neighbors is sufficient to detect any new
overlap.

The vector is mutated only after validation succeeds. Allocation failure from
the standard container can still propagate to the caller; manifest commit wraps
staging in exception handling.

## Removing a table

`remove_table(table_id, optional_level)` supports two modes:

- With a level, it searches only that vector and verifies the id is not
  duplicated there.
- Without a level, it scans the complete catalog and fails if the id is missing
  or appears more than once.

Removal changes only the in-memory metadata. File deletion is an engine action
performed after a manifest edit commits.

## Point-lookup candidates

`find_candidate_tables_in_level(level, key)` uses inclusive table ranges.

For L0 it scans the complete level and returns every matching table in stored
newest-first order:

```text
for table in L0 (descending id):
    if table.smallest_key <= key <= table.largest_key:
        result.push(table)
```

For L1+ it binary-searches for the first table whose `largest_key >= key`, then
checks whether the table's lower bound also contains the key. The result has
zero or one entry.

The returned vector contains `TableMeta` copies. Their `ArenaEntry` key views
still refer to the original arena-owned bytes.

## Range-overlap candidates

`find_overlapping_tables(level, smallest, largest)` returns tables intersecting
the inclusive interval:

```text
overlap(A, B) = !(A.largest < B.smallest) &&
                !(B.largest < A.smallest)
```

An invalid level or reversed query interval returns an empty vector.

- L0 requires a full scan because arbitrary ranges overlap.
- L1+ binary-searches to the first table whose upper bound reaches the query,
  then scans forward until table lower bounds pass the query's upper bound.

The scheduler and manual compaction use this method to include every
destination table touched by a source range.

## Size accounting

`get_layer_size(level)` adds the `file_size` of every table in the requested
level. It uses checked conversion/addition and throws `std::overflow_error` if
the sum cannot fit in `std::size_t`. `CompactionScheduler` has its own
saturating `uint64_t` level-size calculation for pressure decisions.

Other inspection methods are:

```cpp
const std::vector<TableMeta>* get_lx_tables(std::uint32_t level) const;
const std::vector<TableMeta>& levels(std::size_t level) const;
std::uint32_t level_count() const;
bool empty() const;
```

The pointer-returning method uses `nullptr` for an invalid level; `levels()` and
`get_layer_size()` use bounds-checked `vector::at()`.

## Transactional recovery and manifest commits

`swap()` exchanges the complete `levels_` vector. The manifest uses this to
avoid exposing partial state:

```text
copy/rebuild temporary LevelManager
apply all additions and deletions to temporary state
validate counters and catalog
append and sync VersionEdit (for a runtime commit)
swap temporary state into the live manager
```

If validation, allocation, or I/O fails before publication, the live manager
remains unchanged.

## Compaction interaction

`CompactionScheduler` reads the catalog to choose adjacent source and target
levels. A `CompactionPlan` contains copies of all selected `TableMeta` entries.
Before doing I/O, `CompactionJob` verifies that every planned id still exists on
the expected level.

After compaction outputs are published, one manifest edit removes all input
metadata and adds all output metadata. Applying the edit through a staged
manager proves that the new L1+ ranges remain non-overlapping before the edit is
made durable.

## Complexity

Let `N` be the number of tables in the catalog, `n` the number in one level,
and `k` the number of returned overlaps.

| Operation | Complexity |
| --- | --- |
| Add table | `O(N)` duplicate-id check plus vector insertion |
| Remove with level | `O(n)` |
| Remove without level | `O(N)` |
| L0 point candidates | `O(n)` |
| L1+ point candidates | `O(log n)` |
| L0 overlap query | `O(n)` |
| L1+ overlap query | `O(log n + k)` |
| Level byte total | `O(n)` |

## Thread safety

`LevelManager` contains no internal lock. The `Engine` serializes catalog reads,
flushes, compaction selection, and manifest commits with its mutex. Direct users
must provide equivalent external synchronization and must not retain vector
pointers or references across mutations.

## Source

- Interface: [`include/level_manager.h`](../../include/level_manager.h)
- Implementation: [`src/level_manager.cpp`](../../src/level_manager.cpp)
- Tests: [`tests/level_manager_test.cpp`](../../tests/level_manager_test.cpp)
