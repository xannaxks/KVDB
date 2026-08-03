# MemTable

A MemTable is the sorted, in-memory write buffer of an LSM database. Mutations
are appended to the [WAL](./wal.md) first and then inserted into the MemTable.
When the buffer reaches a threshold, it is frozen and written to an immutable
[SSTable](./sstable.md).

The MemTable is a role, not one specific search structure. Production engines
use skip lists, balanced trees, hash-based representations, or specialized
variants depending on the required write, lookup, scan, and memory behavior.
KVDB currently uses a [red-black tree](./red-black-tree.md) as its concrete
backend.

## Responsibilities

KVDB's `MemTable`:

- accepts already versioned `InternalRecord` objects;
- maintains one active mutable red-black tree;
- freezes the active tree into a FIFO queue of immutable generations;
- searches the active and every immutable generation for the newest version;
- pins the oldest immutable generation while an SSTable is built;
- retires a generation only after the engine publishes its SSTable;
- reports approximate memory usage for flush decisions.

It does not assign sequence numbers, write the WAL, publish SSTables, or own the
key/value byte storage. Those responsibilities belong to `Engine`, the WAL and
SSTable layers, and the arena respectively.

## Internal record ordering

The red-black tree orders records by:

```text
user key ascending, sequence number descending
```

Multiple versions of the same user key can coexist, but an identical
`(key, sequence_number)` is a duplicate. A `Put` carries a value; a `Tombstone`
represents deletion and normally carries an empty value.

Ordering equal keys by newest sequence first makes an in-order dump directly
suitable for SSTable construction.

## Generations

The in-memory state has this shape:

```text
new writes
    |
    v
+------------------+
| mutable_table_   |  accepts inserts
+------------------+

+------------------+  front / oldest / next to flush
| immutable #17   |
+------------------+
| immutable #18   |
+------------------+  back / newest
```

Each immutable queue entry contains a monotonically increasing generation id
and a `shared_ptr<const RBTree>`:

```cpp
struct ImmutableTable
{
    std::uint64_t generation_id = 0;
    std::shared_ptr<const RBTree> table;
};
```

The id protects retirement from stale flush work. The shared pointer protects
the tree's lifetime while a builder reads it without requiring the queue entry
to remain present forever.

## Applying mutations

`apply(record)` takes the exclusive MemTable lock and inserts into the active
tree. `put()` and `remove()` are convenience wrappers that construct a `Put` or
`Tombstone` record and call `apply()`.

```text
Engine:
    append record to WAL
    optional WAL sync
    MemTable.apply(record)
```

This call order is outside `MemTable` but is essential: records must not become
visible in memory before their WAL append succeeds.

Record key and value fields are `ArenaEntry` views. Inserting a record copies
the views into a tree node; it does not copy the referenced bytes. The backing
arena must therefore outlive the mutable tree, every immutable snapshot, and
any flush consuming those records.

## Lookup

`get(key)` holds a shared lock and asks every generation for its newest matching
record:

```text
candidate = mutable_table.find_latest_by_key(key)

for immutable from newest to oldest:
    other = immutable.table.find_latest_by_key(key)
    if other.sequence > candidate.sequence:
        candidate = other
```

The explicit sequence comparison makes lookup correct even if recovery or a
test inserts versions outside normal generation order. The normal engine path
assigns sequences monotonically, so a newer generation usually wins without
replacement.

A tombstone is returned as an ordinary successful `InternalRecord`. The caller
must convert it to “not found” and stop searching older SSTables. Returning an
empty optional is reserved for a key absent from every memory generation.

## Freezing the mutable table

`freeze_mutable()` first allocates a replacement `RBTree` before taking the
exclusive lock. This keeps the existing state unchanged if allocation fails.
Under the lock it then:

```text
if active tree is empty:
    return success

append (next_generation_id, active tree) to immutable queue
increment next_generation_id
install preallocated empty tree as mutable
```

The transition is atomic from MemTable readers' perspective. `manual_freeze()`
is currently an alias for the same operation.

Freezing alone does not write a file. `Engine::flush()` freezes and then drains
the immutable queue synchronously.

## Pinning and retirement

`oldest_immutable()` returns an `ImmutableSnapshot` for the queue front:

```cpp
struct ImmutableSnapshot
{
    std::uint64_t generation_id = 0;
    std::shared_ptr<const RBTree> table;
};
```

The snapshot remains valid even after the lock is released. KVDB's current
SSTable builder uses `dump_oldest_immutable()` to materialize this tree in
internal-key order into a vector of non-owning `InternalRecord` objects.

After the table file is durable and the manifest edit commits, the engine calls:

```cpp
retire_oldest_immutable(snapshot.generation_id)
```

Retirement succeeds only if the queue is non-empty and the supplied id still
matches its front. A mismatch returns `false` without removing anything.

The safe lifecycle is therefore:

```text
freeze -> pin/dump -> build and publish SSTable -> commit manifest -> retire
```

Retiring before the manifest commit would remove the in-memory recovery source
too early.

## Memory accounting

`mutable_memory_usage()` reports the active tree's approximate bytes and is
used by `Engine` to trigger a flush when it reaches
`DBOptions::memtable.size_limit`.

`approximate_memory_usage()` adds the active tree and all immutable trees. The
red-black tree estimate includes each node object plus referenced key and value
sizes. It is an operational estimate, not an exact process-resident-memory
measurement; arena page slack and container/control-block overhead are not
fully represented.

## Thread safety

The MemTable uses `std::shared_mutex`:

- `get`, snapshot inspection, and accounting take shared locks;
- insert, freeze, and retire take exclusive locks.

The class is independently thread-safe for these operations. In normal KVDB
use, the outer `Engine` mutex serializes all public calls, so the engine does not
currently exploit concurrent MemTable reads during writes or flushes.

## Complexity

Let `n` be records in one tree and `g` the number of generations.

| Operation | Complexity |
| --- | --- |
| Insert | `O(log n)` |
| Lookup | `O(sum(log n_i))` across `g` trees |
| Freeze | `O(1)` after replacement allocation |
| Pin oldest | `O(1)` |
| Retire oldest | `O(1)` |
| Dump oldest | `O(n)` |
| Memory accounting | `O(total records)` with the current tree implementation |

## Current limitations and research direction

The immutable queue is the structure needed for asynchronous flushing, but
KVDB currently performs flush work synchronously while holding the engine
mutex. `DBOptions::memtable.immutable_tables_limit` is validated but is not yet
used for backpressure or scheduling.

The project intends to compare alternative MemTable structures. Any future
backend must preserve the observable contract used here:

- support multiple versions ordered by key and sequence;
- return the newest record, including tombstones;
- produce a complete internal-key-ordered stream for flush;
- provide stable record views for the lifetime of a frozen generation.

Research notes and candidate structures are collected in
[`RESEARCH.md`](../../RESEARCH.md).

## Source

- Interface: [`include/mem_table.h`](../../include/mem_table.h)
- Implementation: [`src/mem_table.cpp`](../../src/mem_table.cpp)
- Tests: [`tests/mem_table_test.cpp`](../../tests/mem_table_test.cpp)
