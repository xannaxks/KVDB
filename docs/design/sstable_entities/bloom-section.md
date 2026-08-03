# Bloom Section

A [Bloom filter](https://en.wikipedia.org/wiki/Bloom_filter) is a probabilistic
set-membership structure. A valid filter can answer either:

```text
definitely absent  -> skip the SSTable
possibly present   -> continue with the index/data lookup
```

False positives are allowed, but false negatives are not. This makes a Bloom
filter useful when a point read might otherwise inspect many SSTables whose key
ranges contain the requested key.

KVDB stores one Bloom section in every [SSTable](../sstable.md), after the index
section and before metadata.

## Purpose

The section records a checksummed, fixed-format filter derived from every data
record. It is intended to reject negative point lookups without reading a data
block.

KVDB currently builds, writes, loads, and validates this section, but
`SSTable::get()` does not yet call `may_contain()`. Point lookups currently go
directly through the index and lazy data-block view. The on-disk section is
therefore ready for integration but does not yet reduce read I/O.

## Position in the SSTable

```text
... | Index section | padding | Bloom section | padding | Meta section | ...
                         ^
                    block aligned
```

The section starts at a 4096-byte boundary and must fit in one physical block.
Version 1 has a fixed logical size of 153 bytes.

## Disk layout

All numeric fields are little-endian.

```text
+--------------------------------+ 9-byte header
| type = Bloom             (u8)  |
| payload_size = 144       (u32) |
| payload_crc32            (u32) |
+--------------------------------+ 144-byte payload
| bloom_bits = 128         (u64) |
| hash_count = 2           (u32) |
| key_count                (u32) |
| mask[128]                (u8[])|
+--------------------------------+
```

| Part | Encoded size |
| --- | ---: |
| Header | 9 bytes |
| Fixed payload fields | 16 bytes |
| Mask | 128 bytes |
| Complete section | 153 bytes |

The header CRC covers the complete payload and excludes the header itself.

## Version 1 slot representation

The field name `bloom_bits` is historical. In version 1 it is the number of
byte-addressed Boolean slots, not a packed bit count:

```text
bloom_bits = mask.size() = 128
mask[i] is exactly 0 or 1
```

The mask therefore consumes 128 bytes for 128 logical slots. This is simpler to
inspect than a packed bitmap but is eight times larger than a packed
representation with the same number of slots.

`key_count` counts records added to the filter, not distinct user keys. If an
SSTable contains several versions of a key, each version increments the count
and sets the same hash-derived slots again.

## Hashing

Version 1 requires two hashes. The implementation computes two seeded 64-bit
FNV-1a values and uses double hashing:

```text
h(i) = first_hash + i * second_hash
slot = h(i) % bloom_bits
```

For each key, slots for `i = 0` and `i = 1` are set to `1`. Empty keys are
supported. A non-empty key with a null pointer is rejected when building and
treated as “possibly present” during lookup.

## Construction

`rebuild(data_section)` constructs a temporary payload from scratch:

```text
initialize 128 zero slots
set bloom_bits = 128
set hash_count = 2
set key_count = 0

for every data block:
    for every record:
        add record.key

derive header type, payload size, and CRC
publish staged header and payload
```

The staged design is transactional with respect to the in-memory
`BloomSection`: invalid input or allocation/overflow failure preserves the old
filter.

`add_key()` mutates only the payload and does not refresh the header CRC.
Normal SSTable construction uses `rebuild()`. `recompute_crc32()` or `write()`
must be used before serialized state is considered canonical.

## Lookup

`may_contain(key)` checks the two derived slots:

```text
if key pointer or payload state is invalid:
    return true                 # fail open

for each hash:
    if mask[slot] == 0:
        return false            # definitely absent

return true                     # possibly present
```

Failing open is a correctness requirement. Returning `false` from malformed
state could hide a key that really exists. A loaded Bloom section has already
passed CRC validation; `may_contain()` additionally validates payload semantics
before trusting a negative result.

## Write path

`write(file, offset, bloom_offset)`:

1. verifies the tracked file cursor;
2. validates the payload;
3. rebuilds header type, exact payload size, and CRC;
4. pads to a 4096-byte boundary;
5. writes the complete header and payload without crossing that block;
6. publishes `bloom_offset` only after all section bytes succeed.

Derived in-memory header fields are refreshed before output. Physical I/O and
the caller's running offset cannot be rolled back after a partial write, but
the output `bloom_offset` is not changed on failure.

## Load path

`load(file, offset, bloom_offset)` requires a block-aligned section offset and a
complete 153-byte range. It then:

```text
load and validate the 9-byte header
load the fixed payload
recompute and compare payload CRC
validate version-1 payload invariants
commit the caller's offset
```

On failure, the caller-visible offset is restored.

## Validation

A version-1 Bloom section is valid only when:

- the block type is `Bloom`;
- the header payload size is exactly 144;
- `bloom_bits == 128` and equals `mask.size()`;
- `hash_count == 2`;
- every mask byte is `0` or `1`;
- a zero `key_count` has an all-zero mask;
- a non-zero `key_count` has at least one set slot;
- the payload CRC matches the header.

The last key-count rule is a consistency check, not proof that the count is
exact. Bloom filters cannot reconstruct their input cardinality from the mask.

## Current limits

- Parameters are fixed by SSTable format version 1 constants.
- `DBOptions::sstable.bloom_filter` is validated but not yet passed into the
  section builder.
- The mask uses Boolean bytes rather than packed bits.
- There is one whole-table filter, not one filter per data block.
- The current SSTable lookup path does not consult the filter.

## Source

- Interface: [`include/sstable_entities/bloom_section.h`](../../../include/sstable_entities/bloom_section.h)
- Implementation: [`src/sstable_entities/bloom_section.cpp`](../../../src/sstable_entities/bloom_section.cpp)
- Tests: [`tests/sstable_entities/bloom_section_test.cpp`](../../../tests/sstable_entities/bloom_section_test.cpp)
