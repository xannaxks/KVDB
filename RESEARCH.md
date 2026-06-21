# MemTable Candidate Data Structures

This document lists possible data structures that may be evaluated as MemTable
implementations in KVDB.

KVDB is an experimental LSM-based key-value storage engine. Its long-term
research direction is to compare how different in-memory data structures affect
write performance, read performance, memory usage, iteration cost, flush cost,
and implementation complexity.

Not every structure listed here is equally practical. Some are realistic
MemTable candidates, while others are included for comparison, theoretical
interest, or future research.

---

## Selection Criteria

A MemTable data structure should ideally support:

* Ordered key-value storage
* Efficient point lookup
* Efficient insertion
* Ordered iteration
* Range scan support
* Reasonable memory overhead
* Predictable performance
* Practical implementation complexity

For an LSM-based storage engine, ordered iteration is especially important
because the MemTable must eventually be flushed into sorted SSTable files.

---

## Candidate Categories

The candidate structures are grouped into several categories:

1. General-purpose ordered in-memory structures
2. String / byte-key structures
3. Hash-based exact-lookup structures
4. Integer-key predecessor structures
5. Disk / page / storage-oriented indexes
6. Advanced / research-oriented ordered structures
7. Probabilistic / helper structures

Each category has different strengths, limitations, and research value.

---

# 1. General-Purpose Ordered In-Memory Structures

These are the most realistic candidates for MemTable implementations.

They support ordered keys, point lookups, inserts, deletes, and sorted
iteration. This makes them directly compatible with an LSM-style flush path.

## Red-Black Tree

Status: implemented.

A Red-Black Tree is a self-balancing binary search tree. It provides logarithmic
search, insertion, and deletion while using relatively simple balancing rules.

### Why it is relevant

Red-Black Trees are practical, widely used, and relatively easy to implement.
They are a good baseline for ordered MemTable experiments.

### Expected properties

* Ordered iteration: yes
* Range scans: yes
* Insert complexity: `O(log n)`
* Lookup complexity: `O(log n)`
* Memory overhead: moderate
* Implementation difficulty: medium

### Research value

Useful as a baseline ordered tree.

---

## AVL Tree

Status: planned.

An AVL Tree is a height-balanced binary search tree. It maintains stricter
balance than a Red-Black Tree.

### Why it is relevant

AVL Trees may provide faster lookups because of their tighter height bound, but
they may require more rotations during insertion and deletion.

### Expected properties

* Ordered iteration: yes
* Range scans: yes
* Insert complexity: `O(log n)`
* Lookup complexity: `O(log n)`
* Memory overhead: moderate
* Implementation difficulty: medium

### Research value

Useful for comparing strict height balancing against looser balancing.

---

## WAVL Tree

Status: planned.

A WAVL Tree is a rank-balanced binary search tree. It is related to AVL and
Red-Black Trees and can be viewed as a middle ground between them.

### Why it is relevant

WAVL Trees are theoretically interesting because they provide strong balancing
properties while often requiring fewer rotations than AVL Trees.

### Expected properties

* Ordered iteration: yes
* Range scans: yes
* Insert complexity: `O(log n)`
* Lookup complexity: `O(log n)`
* Memory overhead: moderate
* Implementation difficulty: medium/high

### Research value

Useful for comparing rank-balanced trees with classic balanced BSTs.

---

## BB[α] Tree / Weight-Balanced Tree

Status: planned.

A BB[α] Tree, also called a weight-balanced tree, balances nodes based on
subtree sizes rather than height or color.

### Why it is relevant

Weight-balanced trees may behave differently from height-balanced trees under
different insertion patterns.

### Expected properties

* Ordered iteration: yes
* Range scans: yes
* Insert complexity: `O(log n)`
* Lookup complexity: `O(log n)`
* Memory overhead: moderate
* Implementation difficulty: medium/high

### Research value

Useful for studying weight-based balancing compared with height/color/rank
balancing.

---

## Treap / Cartesian Tree

Status: planned.

A Treap combines binary search tree ordering by key with heap ordering by
random priority.

### Why it is relevant

Treaps are simple randomized balanced trees. They may be easier to implement
than AVL or Red-Black Trees while still providing expected logarithmic
performance.

### Expected properties

* Ordered iteration: yes
* Range scans: yes
* Insert complexity: expected `O(log n)`
* Lookup complexity: expected `O(log n)`
* Memory overhead: moderate
* Implementation difficulty: low/medium

### Research value

Useful for comparing deterministic balancing with randomized balancing.

---

## Splay Tree

Status: planned.

A Splay Tree is a self-adjusting binary search tree. Recently accessed nodes
are moved closer to the root.

### Why it is relevant

Splay Trees may perform well under skewed or repeated-access workloads.

### Expected properties

* Ordered iteration: yes
* Range scans: yes
* Insert complexity: amortized `O(log n)`
* Lookup complexity: amortized `O(log n)`
* Memory overhead: moderate
* Implementation difficulty: medium

### Research value

Useful for testing whether access-pattern adaptation helps MemTable workloads.

---

## Scapegoat Tree

Status: planned.

A Scapegoat Tree is a self-balancing binary search tree that occasionally
rebuilds unbalanced subtrees.

### Why it is relevant

Scapegoat Trees avoid storing balancing metadata in every node, but may pay
larger costs during subtree rebuilding.

### Expected properties

* Ordered iteration: yes
* Range scans: yes
* Insert complexity: amortized `O(log n)`
* Lookup complexity: `O(log n)`
* Memory overhead: low
* Implementation difficulty: medium

### Research value

Useful for comparing local rotations with periodic rebuilding.

---

## Skip List

Status: planned.

A Skip List is a probabilistic ordered data structure built from multiple
linked-list levels.

### Why it is relevant

Skip Lists are commonly used in LSM-based storage engines. They provide simple
ordered iteration and expected logarithmic lookup and insertion.

### Expected properties

* Ordered iteration: yes
* Range scans: yes
* Insert complexity: expected `O(log n)`
* Lookup complexity: expected `O(log n)`
* Memory overhead: moderate/high
* Implementation difficulty: medium

### Research value

Very important baseline because many LSM engines use skip-list-like MemTables.

---

## Hash Skip List

Status: planned.

A Hash Skip List combines hash-based lookup ideas with skip-list ordering.

### Why it is relevant

This structure may improve point lookup while preserving some ordered behavior,
depending on the design.

### Expected properties

* Ordered iteration: maybe, depending on implementation
* Range scans: maybe, depending on implementation
* Insert complexity: expected `O(log n)` or better
* Lookup complexity: expected `O(1)` / `O(log n)`, depending on design
* Memory overhead: high
* Implementation difficulty: medium/high

### Research value

Useful as a hybrid exact-lookup/ordered-access experiment.

---

# 2. String / Byte-Key Data Structures

These structures are designed for strings, byte arrays, or prefix-based access.

They may be useful because KVDB keys are byte sequences. However, not all of
them are natural replacements for a general ordered MemTable.

---

## Trie

Status: planned.

A Trie stores keys by characters or bytes rather than by full-key comparison.

### Why it is relevant

Tries can be efficient for prefix-heavy workloads and string keys.

### Expected properties

* Ordered iteration: possible
* Range scans: possible, but implementation-dependent
* Insert complexity: `O(k)`, where `k` is key length
* Lookup complexity: `O(k)`
* Memory overhead: often high
* Implementation difficulty: medium

### Research value

Useful for workloads with many shared prefixes.

---

## Radix Tree

Status: planned.

A Radix Tree compresses chains of single-child Trie nodes.

### Why it is relevant

Radix Trees reduce memory overhead compared with basic Tries.

### Expected properties

* Ordered iteration: possible
* Range scans: possible
* Insert complexity: `O(k)`
* Lookup complexity: `O(k)`
* Memory overhead: medium/high
* Implementation difficulty: medium

### Research value

Useful for comparing byte-wise key traversal against comparison-based trees.

---

## Patricia Trie

Status: planned.

A Patricia Trie is a compressed trie, often used for binary strings or prefixes.

### Why it is relevant

It can provide compact prefix-based storage.

### Expected properties

* Ordered iteration: possible
* Range scans: possible
* Insert complexity: `O(k)`
* Lookup complexity: `O(k)`
* Memory overhead: medium
* Implementation difficulty: medium/high

### Research value

Useful for prefix-compressed key workloads.

---

## Crit-bit Tree

Status: planned.

A Crit-bit Tree branches based on the first differing bit between keys.

### Why it is relevant

It is suitable for byte-string keys and can avoid repeated full-key comparison.

### Expected properties

* Ordered iteration: yes, if implemented carefully
* Range scans: possible
* Insert complexity: `O(k)`
* Lookup complexity: `O(k)`
* Memory overhead: medium
* Implementation difficulty: medium

### Research value

Useful for testing bit-level branching on byte-string keys.

---

## Ternary Search Tree

Status: planned.

A Ternary Search Tree stores characters using less-than, equal, and greater
branches.

### Why it is relevant

It combines properties of binary search trees and tries.

### Expected properties

* Ordered iteration: possible
* Range scans: possible
* Insert complexity: `O(k log σ)` depending on alphabet behavior
* Lookup complexity: `O(k log σ)`
* Memory overhead: medium
* Implementation difficulty: medium

### Research value

Useful for string-key workloads, but probably not a primary MemTable candidate.

---

## Adaptive Radix Tree / ART

Status: planned.

An Adaptive Radix Tree is a high-performance radix tree that changes internal
node representation based on the number of children.

### Why it is relevant

ART is a strong candidate for in-memory indexing over byte keys. It can be very
fast for point lookups and ordered scans.

### Expected properties

* Ordered iteration: yes
* Range scans: yes
* Insert complexity: `O(k)`
* Lookup complexity: `O(k)`
* Memory overhead: medium
* Implementation difficulty: high

### Research value

High. ART is one of the more realistic advanced MemTable candidates.

---

## HAT-Trie

Status: planned.

A HAT-Trie is a cache-conscious trie variant designed for efficient string
storage.

### Why it is relevant

It may provide strong performance for string-heavy workloads.

### Expected properties

* Ordered iteration: possible
* Range scans: possible
* Insert complexity: `O(k)`
* Lookup complexity: `O(k)`
* Memory overhead: medium
* Implementation difficulty: high

### Research value

Useful for cache-conscious string-key experiments.

---

## Burst Trie

Status: planned.

A Burst Trie stores groups of strings in containers and splits them when they
grow too large.

### Why it is relevant

It is designed for efficient string indexing and may reduce per-key overhead.

### Expected properties

* Ordered iteration: possible
* Range scans: possible
* Insert complexity: workload-dependent
* Lookup complexity: workload-dependent
* Memory overhead: medium
* Implementation difficulty: high

### Research value

Useful for string-heavy workloads, but probably not a first-priority candidate.

---

## Masstree

Status: planned.

Masstree is a high-performance trie-of-B+trees design for in-memory indexing.

### Why it is relevant

Masstree is highly relevant to database systems and byte-string keys, but it is
significantly more complex than basic MemTable structures.

### Expected properties

* Ordered iteration: yes
* Range scans: yes
* Insert complexity: approximately logarithmic per layer
* Lookup complexity: approximately logarithmic per layer
* Memory overhead: medium/high
* Implementation difficulty: very high

### Research value

Very high, but probably too complex for the initial version.

---

# 3. Hash-Based Exact-Lookup Structures

Hash-based structures are excellent for point lookups but usually do not
support ordered iteration naturally.

Because LSM flush requires sorted output, these structures are limited as direct
MemTable replacements unless combined with sorting during flush or an additional
ordered structure.

---

## Hash Table

Status: planned.

A Hash Table maps keys to values using a hash function.

### Why it is relevant

Hash tables are extremely strong for point lookup workloads.

### Limitation

A plain hash table does not naturally support ordered iteration or efficient
range scans.

### Expected properties

* Ordered iteration: no
* Range scans: no
* Insert complexity: expected `O(1)`
* Lookup complexity: expected `O(1)`
* Memory overhead: medium
* Implementation difficulty: low/medium

### Research value

Useful as a limited-functionality comparison baseline.

---

## Chained Hash Table

Status: planned.

A Chained Hash Table resolves collisions using linked lists or small containers.

### Research value

Useful for comparing collision-resolution strategies.

---

## Open Addressing Hash Table

Status: planned.

An Open Addressing Hash Table resolves collisions by probing within the table.

### Research value

Useful for measuring cache-friendly exact lookup performance.

---

## Robin Hood Hash Table

Status: planned.

A Robin Hood Hash Table reduces lookup variance by moving entries based on probe
distance.

### Research value

Useful for comparing predictable hash lookup behavior.

---

## Cuckoo Hash Table

Status: planned.

A Cuckoo Hash Table uses multiple hash functions and relocates entries during
insertion.

### Research value

Useful for high-performance exact lookup experiments, but insertion behavior can
be complex.

---

## Hopscotch Hash Table

Status: planned.

A Hopscotch Hash Table keeps entries close to their ideal bucket.

### Research value

Useful for cache-friendly lookup experiments.

---

## Extendible Hashing

Status: planned.

Extendible Hashing grows a directory of buckets dynamically.

### Research value

More relevant to disk/indexing systems than a simple in-memory MemTable.

---

## Linear Hashing

Status: planned.

Linear Hashing grows hash buckets incrementally.

### Research value

Useful mostly as a storage/indexing comparison, not as a first-choice MemTable.

---

## Swiss Table-Style Hash Table

Status: planned.

Swiss Table-style hash tables use metadata bytes and SIMD-friendly probing.

### Research value

Strong exact-lookup baseline, but not naturally ordered.

---

# 4. Integer-Key Predecessor Structures

These structures are designed for integer keys and predecessor/successor
queries.

They are theoretically interesting, but less general for KVDB because KVDB uses
byte-sequence keys.

---

## van Emde Boas Tree

Status: planned.

A van Emde Boas Tree supports predecessor operations over bounded integer
universes.

### Limitation

It requires integer keys from a bounded universe and can have high memory usage.

### Research value

Mostly theoretical unless KVDB has an integer-key benchmark mode.

---

## X-Fast Trie

Status: planned.

An X-Fast Trie supports predecessor queries for integer keys using hashing and
binary tries.

### Research value

Interesting for integer-key workloads, but not a general MemTable replacement.

---

## Y-Fast Trie

Status: planned.

A Y-Fast Trie improves space usage compared with X-Fast Tries.

### Research value

Useful for integer-key predecessor experiments.

---

## Fusion Tree

Status: planned.

A Fusion Tree uses word-level parallelism for integer predecessor queries.

### Research value

Highly theoretical and implementation-heavy. Probably not a practical early
candidate.

---

## Integer Trie

Status: planned.

An Integer Trie indexes integer keys by their binary representation.

### Research value

Useful only for integer-key workloads.

---

## Bitwise Trie

Status: planned.

A Bitwise Trie indexes keys bit by bit.

### Research value

Can support byte/integer keys, but may have high memory overhead.

---

## Binary Trie

Status: planned.

A Binary Trie is a bit-level trie with two branches per bit.

### Research value

Useful for studying bitwise key traversal, but likely not a primary MemTable
candidate.

---

# 5. Disk / Page / Storage-Oriented Indexes

These structures are usually designed for disk pages, block storage, or
database indexes.

Some may be adapted to in-memory MemTables, but many are better understood as
storage-index comparisons rather than direct MemTable candidates.

---

## B-Tree

Status: planned.

A B-Tree stores multiple keys per node and is optimized for block/page access.

### Research value

Useful for comparing page-oriented search trees against pointer-based BSTs.

---

## B+ Tree

Status: planned.

A B+ Tree stores values in leaf nodes and keeps internal nodes as routing
indexes.

### Research value

Very relevant to database systems. More useful as an index structure than as a
classic LSM MemTable, but still worth comparing.

---

## B* Tree

Status: planned.

A B* Tree is a B-Tree variant that tries to keep nodes more densely packed.

### Research value

Useful for studying page utilization and split behavior.

---

## Bε-Tree

Status: planned.

A Bε-Tree uses buffers in internal nodes to batch updates.

### Research value

High theoretical relevance to write-optimized data structures. However, it is
more complex and may overlap conceptually with LSM-style write buffering.

---

## Fractal Tree Index

Status: planned.

A Fractal Tree Index is a write-optimized tree structure related to buffered
tree ideas.

### Research value

High, but complex. Better suited for a later research phase.

---

## Cache-Oblivious B-Tree

Status: planned.

A Cache-Oblivious B-Tree is designed to work well across different cache and
block sizes without explicitly tuning node size.

### Research value

Interesting for cache/memory-layout experiments, but difficult to implement
correctly.

---

## COLA / Cache-Oblivious Lookahead Array

Status: planned.

A COLA stores data across sorted arrays of increasing size.

### Research value

Very relevant to write-optimized storage and LSM-like behavior.

---

## Buffered Repository Tree

Status: planned.

A Buffered Repository Tree is a buffered search tree designed to improve update
performance.

### Research value

Interesting for write-optimized indexing comparisons.

---

## Bw-Tree

Status: planned.

A Bw-Tree is a latch-free B-tree-like index using delta records and mapping
tables.

### Research value

Highly relevant to database internals, but very complex. Probably not a
near-term candidate.

---

## B-link Tree

Status: planned.

A B-link Tree is a B-tree variant with sibling links, often used for concurrent
database indexing.

### Research value

Useful for future concurrency-oriented experiments.

---

## T-Tree

Status: planned.

A T-Tree is an in-memory balanced tree historically used in main-memory
databases.

### Research value

Relevant as an older in-memory database index structure.

---

## 2-3 Tree

Status: planned.

A 2-3 Tree is a balanced search tree where internal nodes may have two or three
children.

### Research value

Mostly educational, because Red-Black Trees are closely related to 2-3-4 style
trees.

---

## 2-3-4 Tree

Status: planned.

A 2-3-4 Tree is a balanced multiway search tree.

### Research value

Useful for understanding the relationship between multiway trees and
Red-Black Trees.

---

## (a,b)-Tree / AB-Tree

Status: planned.

An (a,b)-Tree is a generalized balanced multiway search tree.

### Research value

Mostly theoretical unless used as a generalized B-tree implementation.

---

# 6. Advanced / Research-Oriented Ordered Structures

These structures are mostly included for theoretical or advanced research
interest. They are not first-priority implementation targets.

---

## Tango Tree

Status: planned.

A Tango Tree is a binary search tree designed around dynamic optimality ideas.

### Research value

Theoretically interesting, but probably not practical as an early MemTable
candidate.

---

## Dynamic Optimality BST Variants

Status: planned.

These are BST variants designed to approach optimal performance over access
sequences.

### Research value

Interesting for theory-heavy access-pattern experiments, but difficult to
implement and evaluate fairly.

---

## Cache-Oblivious Search Tree

Status: planned.

A Cache-Oblivious Search Tree aims to perform well across cache levels without
explicit tuning.

### Research value

Relevant to memory-layout experiments.

---

## Packed Memory Array

Status: planned.

A Packed Memory Array stores ordered elements in an array with gaps to allow
efficient insertions.

### Research value

Interesting because it combines ordered layout with cache-friendly scanning.

---

## Judy Array

Status: planned.

A Judy Array is a highly optimized sparse dynamic array/trie-like structure.

### Research value

Useful as a specialized high-performance structure, but difficult to reproduce
fully.

---

# 7. Probabilistic / Helper Data Structures

These are not full MemTable replacements. They are helper structures that can
improve lookup performance or reduce unnecessary reads.

---

## Bloom Filter

Status: planned.

A Bloom Filter is a probabilistic set-membership structure with false positives
but no false negatives.

### Why it is relevant

Bloom filters are commonly used in LSM engines to avoid unnecessary SSTable
lookups.

### Research value

Important helper structure for read-path optimization.

---

## Counting Bloom Filter

Status: planned.

A Counting Bloom Filter supports deletion by storing counters instead of simple
bits.

### Research value

Useful where deletions from the filter are required.

---

## Cuckoo Filter

Status: planned.

A Cuckoo Filter is a probabilistic membership structure that supports deletion.

### Research value

Useful alternative to Bloom filters.

---

## Quotient Filter

Status: planned.

A Quotient Filter stores compact fingerprints and supports membership queries.

### Research value

Useful for comparing compact filter structures.

---

## Xor Filter

Status: planned.

An Xor Filter is a fast and compact static filter.

### Research value

Useful for immutable SSTable-level filtering.

---

## Ribbon Filter

Status: planned.

A Ribbon Filter is a compact approximate membership filter.

### Research value

Useful for studying modern space-efficient filter alternatives.

---

# Practical Priority

The full list above is intentionally broad. For the first serious benchmark
phase, KVDB should focus on a smaller and more realistic set.

## Priority 1: Main MemTable Candidates

These should be implemented and benchmarked first:

* Red-Black Tree
* AVL Tree
* Skip List
* Treap
* Splay Tree

## Priority 2: Advanced but Realistic Candidates

These are valuable after the basic comparison is stable:

* WAVL Tree
* BB[α] / Weight-Balanced Tree
* Scapegoat Tree
* Adaptive Radix Tree
* Bε-tree-inspired MemTable
* COLA-inspired MemTable

## Priority 3: Limited or Specialized Candidates

These are useful for specific workload comparisons:

* Hash Table
* Robin Hood Hash Table
* Swiss Table-style Hash Table
* Trie
* Radix Tree
* Crit-bit Tree
* Integer Trie
* van Emde Boas Tree
* X-Fast Trie
* Y-Fast Trie

## Priority 4: Research / Future Candidates

These are interesting but probably too complex for the first version:

* Masstree
* Fractal Tree Index
* Bw-Tree
* Cache-Oblivious B-Tree
* Tango Tree
* Dynamic optimality BST variants
* Judy Array

---

# Benchmark Dimensions

Each implemented candidate should be evaluated using the same benchmark
interface and workload definitions.

Important metrics:

* Insert throughput
* Point lookup latency
* Range scan latency
* Full ordered iteration time
* Flush-to-SSTable time
* Memory usage
* Allocation count
* Key comparison count
* Rotation/rebalancing count, if applicable
* Cache behavior, if measurable
* Implementation complexity

---

# Workload Types

The benchmark suite should include multiple workload patterns:

## Uniform Random Inserts

Keys are inserted in random order.

Useful for measuring average-case behavior.

## Sequential Inserts

Keys are inserted in sorted order.

Useful for exposing worst-case or balancing behavior.

## Reverse Sequential Inserts

Keys are inserted in descending order.

Useful for testing balancing and insertion robustness.

## Zipfian / Skewed Access

Some keys are accessed much more frequently than others.

Useful for testing structures such as Splay Trees.

## Prefix-Heavy String Keys

Many keys share common prefixes.

Useful for testing Tries, Radix Trees, ART, and related structures.

## Mixed Read/Write Workloads

A combination of inserts, point reads, deletes, and scans.

Useful for approximating real database behavior.

---

# Notes on Fair Comparison

To keep benchmarks fair:

* Use the same key/value generator for every structure.
* Use the same allocator strategy where possible.
* Use the same MemTable interface.
* Use the same flush path into SSTables.
* Separate implementation bugs from structural performance differences.
* Run each benchmark multiple times.
* Report average, median, and tail latency where possible.
* Clearly document hardware, compiler, optimization level, and operating system.

---

# Research Questions

Possible research questions:

* How does MemTable structure affect write throughput?
* How does balancing cost affect insertion performance?
* How does memory layout affect cache behavior?
* Which structures perform best under sequential inserts?
* Which structures perform best under random inserts?
* Which structures perform best under skewed access patterns?
* Which structures are practical replacements for classic skip-list/RB-tree MemTables?
* Which structures are theoretically interesting but impractical in a real engine?
* How much does implementation complexity matter compared with benchmark gains?

---
