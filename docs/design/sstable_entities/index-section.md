# Index Section

The index section maps inclusive user-key ranges to physical
[data-block](./data-section.md) offsets. It lets an [SSTable](../sstable.md)
point lookup select a small candidate set without scanning every record.

## Purpose

There is exactly one index entry per data block. Each entry stores:

```text
physical data-block offset
first user key in that block
last user key in that block
```

Because data is globally ordered, the entries are also ordered by key range and
reference consecutive physical blocks.

## Position in the SSTable

```text
... | Data block N | padding | Index section | padding | Bloom section | ...
                                  ^
                             block aligned
```

The index begins immediately after the data section's reserved blocks and is
aligned to 4096 bytes. Version 1 requires the complete index—header and every
entry—to fit in one physical block.

## Disk layout

```text
+----------------------------------+ 9-byte header
| type = Index               (u8)  |
| payload_size               (u32) |
| payload_crc32              (u32) |
+----------------------------------+ variable payload
| entry 0                          |
| entry 1                          |
| ...                              |
+----------------------------------+
```

Each entry is:

| Size | Field |
| ---: | --- |
| 8 | `data_block_offset` |
| 4 | `first_key_size` |
| 4 | `last_key_size` |
| `first_key_size` | first key bytes |
| `last_key_size` | last key bytes |

The fixed entry width is 16 bytes. All numeric fields are little-endian. The
header CRC covers the complete encoded entry payload and excludes the header.

An empty index is canonical: `payload_size = 0` and the CRC is the CRC32 of an
empty payload.

## Inclusive ranges and shared boundaries

Each entry describes an inclusive range:

```text
[first_key, last_key]
```

Adjacent ranges must be globally ordered, but equality at a boundary is
allowed:

```text
block 0: [a, hot]
block 1: [hot, m]
```

This occurs when many versions of one user key span a block boundary. Because
records are ordered by sequence descending, the earliest candidate block
contains the newest versions.

Ranges may share only this ordered boundary. A previous `last_key` greater than
the next `first_key` is invalid overlap.

## Construction

`DataSection::write()` builds the index while serializing blocks. After a block
is complete it calls:

```cpp
add_index(
    data_block_offset,
    first_record.key,
    last_record.key
);
```

During construction, key pointers borrow bytes owned by the data records. They
must remain valid through index serialization. `add_index()` transactionally
validates and appends one entry, then rebuilds the header.

It requires:

- a 4096-byte-aligned data offset;
- non-null pointers for non-empty keys;
- `first_key <= last_key`;
- exactly `previous_offset + 4096` for the next block;
- globally ordered adjacent ranges;
- total encoded size no greater than 4096 bytes.

## Write path

`write(file, offset, index_offset)` derives a fresh header from the public
entry vector before doing I/O. It then:

```text
validate all entries and exact payload size
compute payload CRC
verify tracked file cursor
align to 4096 bytes
write header and indivisible entries
publish cached header and index_offset
```

Invalid state is rejected before alignment padding. On a physical write
failure, bytes and the running cursor cannot be rolled back, but cached header
state and the caller's `index_offset` are committed only after complete success.

## Load path

The loader requires a block-aligned `index_offset`. It reads and validates the
header, bounds the complete section to one block/file range, reads the encoded
payload into a temporary buffer, and verifies CRC before interpreting entries.

Only after the checksum succeeds does it parse entry lengths, validate range
order and consecutive offsets, allocate one contiguous key area from the
caller's arena, and copy all boundary keys. On failure, both the arena and the
caller-visible offset roll back.

The compatibility loader can optionally compare the encoded section size with
the footer's `index_size`. A strict overload additionally checks entry count and
every referenced offset against `data_offset` and `data_block_count`.

## Candidate search

`find_candidate_range(key)` uses two binary searches:

```text
first = first range whose last_key >= key
end   = first range after first whose first_key > key
```

If `key` is outside the first range, the method returns `NotFound`. Otherwise
it returns `[first, end)`, preserving multiple adjacent candidates that share
the key as a boundary.

`find_first_candidate(key)` returns only `first`. Ordinary latest-value lookup
uses this method because the earliest candidate contains the newest versions.

## Validation

A valid index requires:

- header type `Index`;
- header payload size no greater than `4096 - 9`;
- exact agreement between derived and stored payload size;
- a CRC matching every encoded entry byte;
- aligned, consecutive data-block offsets;
- valid pointers and encoded lengths;
- `first_key <= last_key` for every entry;
- ordered adjacent ranges;
- a complete section no larger than one physical block.

Footer/data-layout validation can additionally require one entry for every
declared data block and an exact first-block offset.

## Complexity

For `b` data blocks:

| Operation | Complexity |
| --- | --- |
| Add entry | `O(b)` because the header/CRC is rebuilt |
| Validate or write | `O(b + boundary-key bytes)` |
| Load | `O(b + boundary-key bytes)` |
| Candidate search | `O(log b)` comparisons |

## Current limits

- The entire index must fit in one 4096-byte block. A table with many blocks or
  large boundary keys can fail construction before other SSTable size limits.
- Boundary keys are stored in full; there is no prefix compression or multi-level
  index.
- The index identifies data blocks, not individual records.

## Source

- Interface: [`include/sstable_entities/index_section.h`](../../../include/sstable_entities/index_section.h)
- Implementation: [`src/sstable_entities/index_section.cpp`](../../../src/sstable_entities/index_section.cpp)
- Tests: [`tests/sstable_entities/index_section_test.cpp`](../../../tests/sstable_entities/index_section_test.cpp)
