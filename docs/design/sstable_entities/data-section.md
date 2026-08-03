# Data Section

The data section stores every versioned key-value record in an
[SSTable](../sstable.md). Records are grouped into independent 4096-byte
physical blocks. Each block has a small header and a checksummed payload; no
record is split across blocks.

KVDB represents this section in two ways:

- `DataSection` is the non-owning construction model used while building a
  table.
- `DataSectionView` is the lazy, bounded read model used after loading an
  immutable table.

## Purpose

Data blocks provide the actual record bytes while preserving these invariants:

```text
records: user key ascending, sequence number descending
blocks:  aligned, independently checksummed, at most 4096 bytes each
index:   exactly one inclusive key range and offset per block
```

Block-level checksums and lazy validation let a read inspect only its candidate
block rather than materializing the entire SSTable.

## Position in the SSTable

```text
File header | padding | Data block 0 | Data block 1 | ... | Index section
                         4096 bytes     4096 bytes
```

Every block begins at a 4096-byte boundary. Used bytes occupy the beginning of
the block; alignment padding fills the unused suffix before the next block or
index section. For a non-empty data section:

```text
index_offset = data_offset + data_block_count * 4096
```

## Data-block layout

```text
+-----------------------------------+ 9-byte block header
| type = Data                 (u8)  |
| payload_disk_size           (u32) |
| payload_crc32               (u32) |
+-----------------------------------+ payload_disk_size bytes
| record 0                          |
| record 1                          |
| ...                               |
+-----------------------------------+
| alignment padding to block end    |
+===================================+
```

The CRC covers the exact serialized payload bytes and excludes the block
header and unused block padding.

## Record layout

Each record has 25 fixed bytes followed by key and value bytes:

| Size | Field | Meaning |
| ---: | --- | --- |
| 4 | `key_size` | Key bytes that follow fixed fields |
| 4 | `value_size` | Value bytes after the key |
| 1 | `type` | `Put` or `Tombstone` |
| 4 | `flags` | Must be zero in version 1 |
| 4 | `reserved` | Must be zero in version 1 |
| 8 | `seq_num` | Record sequence number |
| `key_size` | key | Raw user-key bytes |
| `value_size` | value | Raw value bytes |

All integers are little-endian. One record's encoded size is:

```text
25 + key_size + value_size
```

It must fit beside the 9-byte block header, so the maximum combined key/value
bytes for one record are `4096 - 9 - 25 = 4062`.

## Construction model

`DataSection::Payload` borrows pointers from an `InternalRecord`. It does not
own or copy the key/value bytes. The source arena and records must remain stable
until the data, index, Bloom, and metadata sections have all been derived and
serialized.

`add_payload(record)` validates field widths, pointers, type, and global order.
It appends to the current block if the complete record fits; otherwise it
creates a new block. An oversized record is rejected rather than fragmented.

Equal user keys must have strictly descending sequence numbers. An identical
key and sequence is rejected as a duplicate.

```text
key a, seq 9
key a, seq 7
key b, seq 12
key c, seq 4
```

Ordering is enforced across block boundaries as well as inside one block.

## Derived block header

After each successful append, `DataBlock::rebuild_header()` derives:

```text
type = Data
payload_disk_size = sum(encoded record sizes)
crc32 = CRC(serialized record fields and bytes)
```

The public header is treated as cached derived state. The write path rebuilds
it again before I/O rather than trusting callers not to have changed it.

## Write path and index construction

`DataSection::write(file, offset, index, data_offset)` first validates the
complete record order and rebuilds every block header. It also constructs a
fresh staged `IndexSection`, so repeated write attempts do not append duplicate
index entries.

For each block:

```text
align cursor to 4096 bytes
remember block offset
write header
write every indivisible record payload
add index entry:
    data_block_offset
    first record's key
    last record's key
```

Only after every block succeeds are the output index and `data_offset`
published to the caller. Physical bytes and the running file offset cannot be
rolled back after an I/O failure.

An empty data section is a successful no-op. It clears the derived index,
leaves the cursor unaligned, and sets `data_offset` to the current cursor.
Normal KVDB SSTables are non-empty, but the section format has a canonical
empty representation for tests and lower-level use.

## Three size meanings

The implementation distinguishes:

- **logical size:** sum of every used block header and payload byte;
- **physical span:** from the first block header through the last used byte,
  including inter-block gaps but excluding final padding;
- **reserved span:** `block_count * 4096`, ending at the index section.

For two partially filled blocks:

```text
logical  = used(block 0) + used(block 1)
physical = 4096 + used(block 1)
reserved = 8192
```

Metadata's `data_bytes` stores logical size, not reserved span.

## Lazy load model

`DataSectionView::load()` receives `data_offset`, `data_block_count`, and
normally the trusted `index_offset`. It checks overflow, alignment, file bounds,
and that the declared block count ends at the trusted next-section boundary.

During initial load it reads only each 9-byte block header. For every block it
records:

```text
header_offset
payload_offset
payload_end_offset
next_block_offset
```

It does not read record payload bytes at this stage.

## On-demand block validation

The first operation using a block reads at most its declared payload, verifies
the payload CRC, and parses record boundaries into lightweight `RecordMeta`
entries. Parsing validates:

- complete fixed and variable fields;
- supported record type;
- zero flags and reserved fields;
- key/value ranges inside the declared payload;
- ascending keys and strictly descending sequences for equal keys;
- at least one complete record and exact payload consumption.

Successful validation and parsed metadata are cached. Deterministic corruption
is also cached so later calls return the same result without trusting the block.
Transient I/O failures are not cached and may be retried.

## Point search and record materialization

After the [index section](./index-section.md) selects a candidate block,
`find_first_record()` validates it and binary-searches cached record metadata by
user key. It returns the first equal record, which is the newest because equal
keys are stored in descending sequence order.

The search reads the candidate payload bytes to compare keys. Once a record
index is selected, `read_record()` copies only that record's key and value into
the caller's arena. An arena checkpoint makes this materialization
transactional: failed allocation or I/O restores the arena.

## Validation and corruption handling

Construction rejects invalid state before output where possible. Loading and
lazy access reject:

- unaligned or overflowing block ranges;
- a payload larger than `4096 - 9`;
- truncated headers, records, keys, or values;
- wrong block type;
- checksum mismatch;
- invalid record fields or internal-key order;
- block/record indexes outside the discovered view.

The footer, data view, index, and metadata provide independent cross-checks for
section extent, block count, offsets, and key boundaries.

## Current limits

- Block size is fixed at 4096 bytes.
- Records cannot span data blocks, so large key/value pairs are rejected.
- Key and value sizes are 32-bit.
- There is no compression or restart-point encoding.
- The loaded view is lazy but point search rereads candidate payload bytes after
  validation; payload contents are not retained in a block cache.

## Source

- Construction interface: [`include/sstable_entities/data_section.h`](../../../include/sstable_entities/data_section.h)
- Lazy-view interface: [`include/sstable_entities/data_section_view.h`](../../../include/sstable_entities/data_section_view.h)
- Construction implementation: [`src/sstable_entities/data_section.cpp`](../../../src/sstable_entities/data_section.cpp)
- Lazy-view implementation: [`src/sstable_entities/data_section_view.cpp`](../../../src/sstable_entities/data_section_view.cpp)
- Tests: [`tests/sstable_entities/data_section_test.cpp`](../../../tests/sstable_entities/data_section_test.cpp) and [`data_section_view_test.cpp`](../../../tests/sstable_entities/data_section_view_test.cpp)
