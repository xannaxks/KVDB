# KVDB Architecture

KVDB is an experimental C++23 key-value database built around a
[Log-Structured Merge-tree (LSM-tree)](https://en.wikipedia.org/wiki/Log-structured_merge-tree).
This document first explains the architecture shared by LSM databases and then
maps those ideas to the components and guarantees implemented by KVDB.

> KVDB is an educational and experimental storage engine. Its file formats and
> public API are not stable, and it is not intended for production data.

## LSM-tree architecture

An LSM database turns random user updates into sequential writes. A mutation is
first made durable in a write-ahead log and inserted into a sorted in-memory
table. When that table reaches a threshold, its contents are written as a new,
immutable sorted file. Background or synchronous compaction later merges those
files into progressively larger levels.

```text
                              memory
                         +-------------+
put/delete -> WAL ------>|  MemTable   |
                         +-------------+
                                |
                              freeze
                                v
                         +-------------+
                         | immutable   |
                         | MemTable(s) |
                         +-------------+
                                |
                              flush
                                v
disk                 L0: [SST] [SST] [SST]     ranges may overlap
                                |
                             compact
                                v
                     L1: [   SST   ] [   SST   ] ranges do not overlap
                                |
                             compact
                                v
                     L2: [       SST       ] ...
```

### Records and ordering

LSM engines normally store an internal record rather than only a user key and
value. The internal record adds a monotonically increasing sequence number and
a record type:

```text
InternalRecord = (user_key, value, type, sequence_number)

type = Put | Tombstone
```

Records are ordered by:

```text
user key ascending, sequence number descending
```

The descending sequence order puts the newest version of one key first. A
delete does not immediately remove older bytes. It writes a tombstone, which
hides earlier values until compaction can prove that the tombstone and all
covered versions are safe to discard.

### Write path

A typical LSM write follows this order:

```text
1. Assign a sequence number.
2. Append the mutation to the WAL.
3. Synchronize the WAL when the selected durability policy requires it.
4. Insert the same versioned record into the mutable MemTable.
5. Acknowledge the write.
```

Writing the log before changing memory is the central recovery invariant. If a
process stops before the MemTable is flushed, the durable WAL prefix can rebuild
it. Sequential WAL appends and later batched SSTable creation avoid a random
disk write for every user mutation.

### Flush path

When the MemTable becomes large, the engine freezes it. The frozen generation
remains readable but accepts no more writes, while a new mutable generation can
take over. The immutable records are already sorted, so the engine can write
them sequentially into an SSTable and install that table in level zero (L0).

The important publication order is:

```text
build SSTable -> sync and publish file -> commit metadata -> retire memory/WAL
```

Publishing metadata before the table file is durable can leave recovery
pointing at an incomplete file. Retiring the MemTable or WAL before metadata is
durable can lose acknowledged data.

### Read path

Because a key can have versions in several places, point lookup checks sources
from newest to oldest:

```text
mutable MemTable
newest immutable MemTable -> oldest immutable MemTable
newest overlapping L0 SSTable -> oldest overlapping L0 SSTable
candidate SSTable in L1
candidate SSTable in L2
...
```

A hit containing a tombstone terminates the search just like a value hit; it
means older sources must not be consulted. SSTable indexes narrow a lookup to a
data block, and Bloom filters can reject keys that are definitely absent before
that block is read.

### Levels and compaction

L0 is special. Each flush creates a new file and its key range may overlap any
existing L0 file. Higher levels are normally maintained as sorted,
non-overlapping ranges, so a point lookup needs at most one table per such
level.

Compaction selects one or more source tables and every overlapping table in the
next level. It performs a sorted merge, resolves multiple versions of each user
key, writes replacement SSTables, and atomically changes the active table set.
Old input files are deleted only after the metadata change is durable.

Without snapshots, compaction can keep only the newest version of each key. A
tombstone must remain while an older value could exist in a lower level; it can
be dropped at a bottommost destination.

### Recovery metadata

SSTable files alone do not say which files form the active database version.
An LSM database therefore keeps a manifest (or version log) containing metadata
edits such as table additions, table deletions, and allocator counters. Startup
replays the manifest to rebuild the level catalog, then replays the current WAL
to reconstruct unflushed records.

### LSM trade-offs

LSM trees exchange cheap sequential writes for deferred maintenance:

- **Write amplification:** compaction may rewrite the same record at several
  levels.
- **Read amplification:** a lookup may consult memory and multiple SSTables.
- **Space amplification:** obsolete versions coexist until compaction removes
  them.
- **Compaction cost:** merging consumes I/O and CPU and can create latency
  spikes when it runs inline.

MemTable size, level thresholds, Bloom-filter quality, table size, and
compaction scheduling balance these costs.

## KVDB component map

`KVDB` is the public interface. `Engine` is its concrete implementation and the
owner/coordinator of the storage components.

```text
                         +----------------------+
client ---------------->| KVDB / Engine API    |
                         +----------+-----------+
                                    |
                  +-----------------+------------------+
                  |                 |                  |
                  v                 v                  v
              +-------+       +-----------+      +-----------+
              |  WAL  |       | MemTable  |      | Manifest  |
              +-------+       |  RBTree   |      +-----+-----+
                              +-----+-----+            |
                                    |                  v
                                    |           +--------------+
                                    +---------->| LevelManager |
                                                +------+-------+
                                                       |
                       +-------------------------------+------+
                       |                                      |
                       v                                      v
              +----------------+                    +----------------+
              | SSTableManager |<------------------>| Compaction     |
              | + cache        |                    | scheduler/job  |
              +-------+--------+                    +----------------+
                      |
                      v
             immutable SSTable files
```

The main responsibilities are:

- [`Engine`](./design/engine.md) validates options, serializes public
  operations, performs recovery, and coordinates reads, writes, flushes, and
  compactions.
- [`WAL`](./design/wal.md) stores versioned mutations in checksummed,
  block-bounded fragments.
- [`MemTable`](./design/mem_table.md) owns one mutable red-black tree and a FIFO
  queue of immutable generations.
- [`SSTable`](./design/sstable.md) is the immutable, block-oriented table
  format. Its data is ordered by key ascending and sequence descending.
- [`Manifest`](./design/manifest.md) durably records `VersionEdit` transitions
  and the next table, sequence, and WAL identifiers.
- [`LevelManager`](./design/level-manager.md) is the in-memory table catalog and
  enforces L0 and L1+ range invariants.
- `SSTableManager` assigns canonical names, builds and loads tables, and caches
  loaded immutable table objects.
- `CompactionScheduler` chooses work from level pressure;
  `CompactionJob` merges inputs and returns an uncommitted manifest edit.
- [`Arena`](./design/arena.md) owns the key/value bytes referenced by
  `ArenaEntry` and `InternalRecord` objects.

## Persistent directory layout

One database directory currently has this shape:

```text
<db_path>/
|-- MANIFEST
|-- wal-000000001.log
`-- sstables/
    |-- table-000000001.sst
    |-- table-000000002.sst
    `-- table-000000003.sst.tmp   # only while a table is being built
```

Table and WAL identifiers are monotonically allocated. Table id zero is
reserved. The manifest identifies the active WAL generation and active table
set; directory contents alone are not authoritative.

## KVDB write path

`put()` and `remove()` take the engine mutex and call the same internal
mutation path. A delete uses `Type::Tombstone` and an empty value.

```text
validate engine state and sequence capacity
        |
copy key/value into the Engine arena
        |
construct InternalRecord(next_sequence)
        |
append to current WAL
        |
optional WAL sync (DBOptions::wal.sync_on_write)
        |
insert into mutable RBTree
        |
increment next_sequence
        |
flush synchronously if MemTable or WAL threshold is reached
```

The WAL precedes the MemTable, so a mutation never becomes visible in memory
before its log append succeeds. With `sync_on_write = false` (the default), a
successful call means the record was appended through the file abstraction but
does not request an `fsync` for every mutation. Flush and close synchronize the
WAL.

A flush is triggered when either:

```text
mutable MemTable approximate bytes >= memtable.size_limit
current WAL offset                 >= wal.file_size_limit
```

## KVDB read path

All public reads also take the engine mutex. Lookup proceeds as follows:

```text
1. MemTable::get searches mutable and immutable generations and returns the
   highest-sequence in-memory record.
2. If memory has a value or tombstone, stop.
3. For each level from L0 downward, ask LevelManager for candidate tables.
4. Open each candidate through SSTableManager's shared cache.
5. Use the SSTable index to select the earliest candidate data block.
6. Lazily validate that block, binary-search its record metadata, and copy the
   newest matching record into a temporary read arena.
7. Convert a Put to std::string or a Tombstone to an empty optional.
```

L0 candidates are returned newest-first by descending table id. L1 and higher
contain non-overlapping inclusive ranges and return at most one candidate per
level.

KVDB writes and validates a Bloom section in every SSTable, but the current
`SSTable::get()` implementation does not call `BloomSection::may_contain()`.
Point lookup therefore uses the index and data-block view directly. See the
[Bloom-section design](./design/sstable_entities/bloom-section.md).

## KVDB flush and publication

`flush()` freezes a non-empty mutable tree and drains immutable generations
oldest-first:

```text
freeze mutable RBTree
        |
pin oldest immutable generation
        |
build table-N.sst.tmp from its sorted records
        |
write sections, sync file, rename to table-N.sst, sync directory
        |
derive TableMeta for L0
        |
optionally create and sync replacement WAL
        |
commit VersionEdit to MANIFEST
        |
retire the exact immutable generation id
        |
switch WAL generation and delete old WAL when safe
        |
run threshold compactions, if enabled
```

The manifest commit stages changes against a copy of the level catalog,
appends and synchronizes the edit, and only then publishes the staged in-memory
state. This is the database's metadata commit point.

## KVDB compaction

The scheduler gives L0 pressure priority. When the L0 file count reaches its
trigger, all L0 tables are selected along with overlapping L1 tables. For an
over-budget L1+ source level, the largest table is selected, with table id as a
tie-breaker, plus every overlapping table in the next level.

`CompactionJob` rechecks that every input still exists, opens an iterator for
each table, and performs a heap-based k-way merge in internal-key order. KVDB
currently has no snapshots, so it keeps only the newest record for each user
key. A bottommost tombstone is dropped; other tombstones are retained. Output
files roll when their approximate size reaches the destination level's target.

The job returns a `VersionEdit` that adds outputs, deletes inputs, and advances
the next table id. `Engine` commits the edit before deleting obsolete input
files.

`compact_range(begin, end)` first flushes memory, then compacts overlapping
tables level by level. The current overlap implementation treats both range
bounds as inclusive and expands the plan until all connected source and target
ranges are covered.

## Startup and recovery

`KVDB::open()` returns an instance only after the complete open sequence
succeeds:

```text
validate DBOptions
create/validate database and sstables directories
allocate Arena, LevelManager, MemTable, managers, and scheduler
load or create MANIFEST
    replay valid VersionEdits into LevelManager
    truncate a recoverable torn manifest tail before append
load or create the manifest-selected WAL
    validate header and fragments
    replay complete records into MemTable
    accept an incomplete final record as a torn tail
    reject corruption in the durable prefix
rewrite recovered records into a new WAL generation
commit new WAL id and next sequence to MANIFEST
remove the old WAL
publish Engine as open
```

Recovered record bytes and loaded metadata keys are copied into the engine
arena. Recovery updates `next_sequence` to be greater than every replayed
sequence number.

## Concurrency and ownership

`Engine` protects every public operation and statistics access with one mutex.
This makes the public database interface thread-safe, but reads, writes,
flushes, and compactions are serialized. `MemTable` has its own shared mutex so
it can safely expose generation operations independently, although the current
engine-level locking prevents concurrent engine reads during maintenance.

The option named `enable_background_compaction` currently enables automatic
compaction checks; it does not start a worker thread. Automatic compaction runs
synchronously in the caller that triggered a flush.

Most internal records are non-owning views. The engine arena owns mutation,
recovery, index, and metadata key bytes for the engine lifetime. Immutable
MemTable snapshots use `shared_ptr` to pin their red-black tree until a flush is
durably published. Point reads use a short-lived arena and copy the result into
an owning `std::string` before returning.

## Durability boundaries

KVDB uses three related but distinct durable structures:

- The WAL protects recent, unflushed mutations.
- SSTables protect immutable sorted data and are published with a temporary
  file, file sync, atomic rename, and parent-directory sync.
- The manifest decides which SSTables and WAL generation belong to the active
  database version.

The ordering between them is more important than any one file:

```text
write:       WAL append -> MemTable apply
flush:       SSTable publish -> manifest commit -> MemTable retire/WAL cleanup
compaction:  output publish -> manifest commit -> input deletion
recovery:    manifest replay -> current WAL replay
```

## Current scope and limitations

The current architecture deliberately keeps several production concerns out of
scope:

- no snapshots, transactions, write batches, or range iterators in the public
  API;
- no background flush or compaction worker; maintenance is synchronous;
- one engine mutex serializes all public operations;
- the MemTable backend is currently an `RBTree` even though alternative
  structures are a research goal;
- Bloom data is persisted but not yet consulted by `SSTable::get()`;
- the SSTable v1 index must fit in one 4 KiB block;
- WAL and SSTable format block sizes and Bloom parameters are compile-time v1
  constants; similarly named `DBOptions` fields are validated but are not yet
  propagated into those encoders;
- `immutable_tables_limit`, manifest rewrite threshold, and lazy-loading policy
  are configuration placeholders rather than active scheduling policies;
- `close()` synchronizes and closes the WAL and manifest but does not force a
  MemTable flush; unflushed state is recovered from the WAL on the next open;
- obsolete or orphan temporary files are not managed by a general-purpose
  garbage collector.

These constraints keep the implementation small enough to study while making
the main LSM invariants—log-before-memory, immutable table publication,
manifest-controlled versions, ordered lookup, and compaction—explicit.
