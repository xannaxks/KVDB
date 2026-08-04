# Roadmap

This roadmap describes the planned development direction of KVDB.

KVDB is an experimental C++ LSM-based key-value storage engine. The current
focus is correctness and core storage-engine behavior. The long-term goal is
to benchmark different MemTable data structures inside the same engine.

## Current Status

- Arena allocator implemented
- Internal record representation implemented
- Red-Black Tree MemTable implemented
- WAL implemented
- SSTable writer/reader implemented
- Level manager partially implemented
- Manifest/version edit format partially implemented
- Documentation partially written

KVDB is still experimental and not production-ready.

## Phase 1: Core Storage Engine

- [x] Arena allocator
- [x] Internal record representation
- [x] MemTable interface
- [x] Red-Black Tree MemTable
- [x] Write-Ahead Log
- [x] SSTable writer
- [x] SSTable reader
- [x] Basic manifest/version edit format
- [x] Stable `put(key, value)` API
- [x] Stable `get(key)` API
- [x] Delete/tombstone support in read path

## Phase 2: LSM Tree Behavior

- [x] Flush MemTable to SSTable
- [x] Level manager integration
- [x] Compaction picker
- [x] Minor compaction
- [x] Major compaction
- [x] Overlap detection between levels
- [x] Manifest recovery
- [x] Crash recovery from WAL + manifest

## Phase 3: Testing and Reliability

### Arena allocator
- [x] Unit tests
- [x] Integration tests

### Initial MemTable data structure (Red-Black Tree)
- [x] Unit tests
- [x] Integration tests

### WAL

- [x] Unit tests
- [ ] Integration tests
- [x] Corruption tests
- [ ] Crash-safety tests
- [x] Partial-write tests

### Manifest

- [x] Unit tests
- [ ] Integration tests
- [x] Corruption tests
- [ ] Crash-safety tests

### SSTable

- [x] Unit tests
- [ ] Integration tests
- [x] Corruption tests
- [ ] Fuzz tests

### File Layer

- [x] Unit tests
- [x] Integration tests

### Helpers

- [x] Unit tests:
	- [x] Little Endian helpers 
	- [x] File helpers
	- [x] Arena helpers
	- [x] CRC helpers
	
- [ ] Integration tests:
	- [x] Little Endian helpers 
	- [x] File helpers
	- [x] Arena helpers
	- [x] CRC helpers

## Phase 4: Benchmarking

- [x] Benchmark write throughput
- [x] Benchmark read latency
- [ ] Benchmark memory usage
- [x] Benchmark flush performance
- [x] Benchmark recovery time
- [x] Define benchmark methodology
- [x] Add reproducible benchmark scripts

## Phase 5: Documentation

- [x] Arena allocator documentation
- [x] Red-Black Tree documentation
- [x] MemTable documentation
- [x] WAL documentation
- [x] SSTable documentation
- [x] Manifest documentation
- [x] Level manager documentation
- [x] Table meta documentation
- [x] Compaction documentation
- [x] Benchmark methodology

## Phase 6: MemTable Experiments

Initial candidates:

- [x] Red-Black Tree
- [ ] AVL Tree
- [ ] Skip List
- [ ] Treap
- [ ] Splay Tree

Extended candidates:

- [ ] WAVL Tree
- [ ] BB[α] / Weight-Balanced Tree
- [ ] Scapegoat Tree
- [ ] Adaptive Radix Tree
- [ ] Bε-tree-inspired MemTable

The full list of possible candidate data structures is documented separately
in [`RESEARCH.md`](./RESEARCH.md).

## Long-Term Research Direction

The long-term goal is to evaluate how different in-memory ordered data
structures affect the performance of an LSM-based key-value engine.

Possible research questions:

- How does MemTable structure affect write throughput?
- How does balancing cost affect flush performance?
- How does memory layout affect cache behavior?
- Which structures are practical replacements for classic skip-list/RB-tree MemTables?
- Which structures are theoretically interesting but impractical in a real engine?