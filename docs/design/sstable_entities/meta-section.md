# Meta Section

The metadata section is a checksummed summary of an
[SSTable's](../sstable.md) data and index. It stores record statistics,
sequence bounds, user-key bounds, data-block count, and logical data bytes.

The section is derived from the table contents. It is not an independent source
of truth that callers should edit by hand.

## Purpose

Metadata supports table catalog creation and structural cross-checking without
rescanning every record. [`make_table_meta()`](../../../src/table_meta.cpp)
combines this summary with the file header/footer and published path to create
the `TableMeta` stored in the manifest and level manager.

## Position in the SSTable

```text
... | Bloom section | padding | Meta section | padding | File footer
                                   ^
                              block aligned
```

The section starts at a 4096-byte boundary and must fit entirely in one
physical block.

## Disk layout

```text
+----------------------------------+ 9-byte header
| type = Meta                (u8)  |
| payload_size               (u32) |
| payload_crc32              (u32) |
+----------------------------------+ payload
| record_count               (u64) |
| tombstone_count            (u64) |
| min_seq_num                (u64) |
| max_seq_num                (u64) |
| min_key_size               (u32) |
| max_key_size               (u32) |
| data_block_count           (u64) |
| data_bytes                 (u64) |
| minimum key bytes                |
| maximum key bytes                |
+----------------------------------+
```

All numeric fields are little-endian.

| Part | Encoded size |
| --- | ---: |
| Header | 9 bytes |
| Fixed payload fields | 56 bytes |
| Boundary keys | `min_key_size + max_key_size` |
| Minimum complete section | 65 bytes |

The header CRC covers the exact payload bytes and excludes the header.

## Field semantics

- `record_count` counts every stored version, not distinct user keys.
- `tombstone_count` counts records whose type is `Tombstone`.
- `min_seq_num` and `max_seq_num` bound all record sequence numbers.
- `min_key` and `max_key` are lexicographic user-key bounds.
- `data_block_count` must equal the number of index entries.
- `data_bytes` is the sum of used bytes in every data block, including each
  9-byte block header but excluding inter-block alignment padding.

Thus `data_bytes` is a logical byte count. It is normally smaller than the
reserved data extent `data_block_count * 4096`.

## Construction

`rebuild(data_section, index_section)` scans all data blocks into a temporary
payload:

```text
for each block:
    require a non-empty, valid-size block
    add block.disk_size to data_bytes

    for each record:
        increment record_count
        count tombstones
        widen sequence range
        widen user-key range

require data block count == index entry count
cross-check minimum/maximum key against index boundaries
validate staged payload
derive payload_size and CRC
publish staged metadata
```

Rebuild is transactional. Invalid records, impossible sizes, mismatched index
state, or overflow leave the previous `MetaSection` unchanged.

While building, `min_key_ptr` and `max_key_ptr` borrow bytes from data records.
Those bytes must outlive metadata serialization.

## Canonical empty metadata

A zero-record payload is valid only when all derived values are zero:

```text
record_count = tombstone_count = 0
min_seq_num = max_seq_num = 0
min_key_size = max_key_size = 0
data_block_count = data_bytes = 0
```

Any partially populated “empty” summary is rejected. Normal published KVDB
SSTables are built from non-empty inputs, but the canonical empty form keeps the
section API well-defined.

## Write path

`write(file, offset, meta_offset)`:

1. validates the payload and derives a fresh header;
2. verifies the explicit file cursor;
3. aligns to a 4096-byte boundary;
4. writes the 9-byte header and indivisible payload;
5. publishes cached header state and `meta_offset` after complete success.

Invalid state is rejected before padding. Physical bytes and the running offset
cannot be undone after a partial I/O failure, but output metadata is not
committed early.

## Load path

`load(file, offset, index, arena, meta_offset)` requires a block-aligned
`meta_offset`. It reads the header, fixed fields, and key lengths; verifies that
the declared payload size equals `56 + min_key_size + max_key_size`; then copies
both boundary keys into one arena allocation.

After reading, it verifies payload CRC, payload invariants, index entry count,
and index boundary keys. Any failure restores the caller's original offset and
arena checkpoint.

Loaded key pointers refer to the supplied arena rather than the file buffer.
That arena must outlive the loaded SSTable metadata and any `TableMeta` derived
from it.

## Validation

A valid non-empty metadata payload requires:

- block type `Meta`;
- exact payload size and matching CRC;
- complete section size no greater than 4096 bytes;
- non-null boundary pointers for non-empty keys;
- `tombstone_count <= record_count`;
- `data_block_count > 0` and `data_block_count <= record_count`;
- `min_seq_num <= max_seq_num`;
- `min_key <= max_key`;
- a plausible `data_bytes` value between the minimum encoded block/record bytes
  and `data_block_count * 4096`;
- data-block count equal to index entry count;
- minimum and maximum keys equal to the observed index boundaries.

The index cross-check prevents a validly checksummed metadata payload from
claiming different key ranges than the table's lookup structure.

## Current limits

- Metadata and both full boundary keys must fit in one 4096-byte block.
- Counts and sequence bounds are 64-bit; key lengths are 32-bit.
- There is no compression, custom comparator id, timestamp, or per-column
  metadata in format version 1.
- The summary describes stored record versions, not live distinct keys after
  tombstone/version resolution.

## Source

- Interface: [`include/sstable_entities/meta_section.h`](../../../include/sstable_entities/meta_section.h)
- Implementation: [`src/sstable_entities/meta_section.cpp`](../../../src/sstable_entities/meta_section.cpp)
- Tests: [`tests/sstable_entities/meta_section_test.cpp`](../../../tests/sstable_entities/meta_section_test.cpp)
