# Write-Ahead Log (WAL)

A write-ahead log is an append-only recovery file. A database records a
mutation in the WAL before making that mutation visible in volatile memory. If
the process stops before the MemTable is flushed to an SSTable, startup replays
the durable WAL prefix to reconstruct it.

In KVDB the WAL works with the [MemTable](./mem_table.md),
[SSTables](./sstable.md), and [Manifest](./manifest.md):

```text
put/delete -> WAL append -> optional sync -> MemTable
                                           |
                                         flush
                                           v
                                        SSTable
```

The WAL contains user mutations. The manifest contains metadata transitions;
the two logs are not interchangeable.

## Files and generations

WAL files use monotonically increasing ids and canonical names:

```text
<db_path>/wal-000000001.log
<db_path>/wal-000000002.log
```

The manifest's `current_wal_id` chooses the generation to recover. The id is
also stored inside the WAL header so renaming or selecting the wrong file is
detected.

Creating a WAL is destructive for an existing file at the same path: the
writer removes it, opens a fresh file, writes the header, and synchronizes that
header. Recovery therefore never reopens an old WAL through `create()`.

## Version 1 physical layout

The format uses fixed 4 KiB physical blocks and little-endian integers.

```text
+-----------------------------+ offset 0
| WAL file header (40 bytes)  |
+-----------------------------+
| fragment header (22 bytes)  |
| fragment payload            |
+-----------------------------+
| fragment header             |
| fragment payload            |
| ...                         |
+-----------------------------+
| 0xCD padding, when needed   | end of 4 KiB block
+=============================+ next block
| next fragment ...           |
+-----------------------------+
```

No fragment crosses a block boundary. If the remaining bytes cannot hold a
22-byte header plus at least one payload byte, the writer pads to the next block
before writing the fragment. Readers perform the same boundary calculation and
skip that padding.

## WAL file header

The fixed header is 40 bytes:

| Offset | Size | Field | Meaning |
| ---: | ---: | --- | --- |
| 0 | 4 | `magic` | `0x4B565741` (`WAL_FILE_MAGIC`) |
| 4 | 4 | `version` | Format version, currently `1` |
| 8 | 4 | `header_size` | Must be `40` |
| 12 | 8 | `wal_id` | WAL generation identity |
| 20 | 8 | `start_seq` | Sequence associated with generation start |
| 28 | 4 | `block_size` | Must be `4096` |
| 32 | 4 | `reserved` | Must be zero |
| 36 | 4 | `header_crc32` | CRC of all preceding header fields |

Equivalent in-memory structure:

```cpp
struct WALFileHeader
{
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint64_t wal_id;
    std::uint64_t start_seq;
    std::uint32_t block_size;
    std::uint32_t reserved;
    std::uint32_t header_crc32;
};
```

Loading verifies the complete header, expected WAL id, supported version and
block size, zero reserved field, and CRC before returning it. Empty or truncated
headers are not valid WALs.

## Logical record encoding

One `InternalRecord` is encoded into a logical payload before fragmentation:

```text
+------------------+ 4 bytes
| key_size (u32)   |
+------------------+ 4 bytes
| value_size (u32) |
+------------------+ key_size bytes
| key bytes        |
+------------------+ value_size bytes
| value bytes      |
+------------------+
```

The record type (`Put` or `Tombstone`) and sequence number live in every
fragment header, not in this payload. A logical payload is valid only when its
encoded length is exactly `8 + key_size + value_size`.

The engine uses an empty value for tombstones. Empty keys and values are valid
at the format layer; a non-zero length with a null pointer is rejected.

## Fragment header

Each physical fragment has a 22-byte header:

| Offset in header | Size | Field | Meaning |
| ---: | ---: | --- | --- |
| 0 | 4 | `fragment_crc32` | CRC of the remaining header fields and payload |
| 4 | 4 | `header_size` | Must be `22` |
| 8 | 1 | `fragment_type` | `FULL`, `FIRST`, `MIDDLE`, or `LAST` |
| 9 | 1 | `record_type` | `Put` or `Tombstone` |
| 10 | 8 | `seq_num` | Logical record sequence number |
| 18 | 4 | `fragment_size` | Payload bytes in this fragment |

CRC input is the canonical serialized form of:

```text
header_size | fragment_type | record_type | seq_num | fragment_size | payload
```

The checksum field itself is excluded. The loader also verifies that the
declared payload fits in the current block and is fully present in the file.

## Fragmentation

If the complete logical payload fits in the current block, the writer emits one
`FULL` fragment. Otherwise it emits:

```text
FIRST -> zero or more MIDDLE fragments -> LAST
```

Every fragment of one logical record repeats the same record type and sequence
number. The writer fills each block's remaining payload capacity before moving
to the next block.

Example:

```text
block N:     [FIRST header][first payload slice][padding if any]
block N + 1: [MIDDLE header][middle payload slice]
block N + 2: [LAST header][final payload slice]
```

The recovery assembler rejects:

- `MIDDLE` or `LAST` without a preceding `FIRST`;
- `FULL` or another `FIRST` while a record is incomplete;
- sequence or record-type changes inside a fragment chain;
- an unknown fragment or record type;
- a reconstructed payload whose length fields are inconsistent.

## Writer

`WALWriter` tracks the file path, WAL id, writable file, and exact append
offset. Every operation verifies that its tracked offset matches the underlying
file cursor.

```text
create(path, id, start_seq):
    replace path
    write canonical header
    sync header

write(record):
    validate and encode logical payload
    split payload at block boundaries
    checksum and append each fragment

sync():
    synchronize current file

rotate(new_path, new_id, start_seq):
    sync and close current file
    create new generation

close():
    sync, close, and clear writer state
```

Fragment append is sequential, but a physical I/O failure may leave a partial
tail. The recovery protocol classifies that tail rather than assuming every
write call is atomic.

## Batch recovery

`WALLoader::load()` reads the header and consumes fragments until EOF or a
terminal condition. Complete logical records are decoded into `InternalRecord`
objects whose key and value bytes are copied into the caller's arena.

Recovery maintains:

```text
last_good_offset = end of last complete logical record
record_checkpoint = arena state after last complete logical record
```

If EOF occurs inside a fragment or before a required `LAST`, recovery rolls the
arena and offset back to this boundary and reports:

```text
ok = true
had_torn_tail = true
```

The valid prefix is still replayable. In contrast, checksum failure, invalid
fragment order, invalid format fields, or an invalid logical payload reports:

```text
ok = false
had_corruption = true
```

Ordinary I/O or allocation failures are returned directly rather than being
misclassified as corruption.

## Streaming recovery

`WALStreamingLoader` exposes one complete logical record per `load_next()`
call. It loads the file header once, preserves `last_good_offset`, and keeps a
terminal state:

```text
Closed -> Ready -> EndOfFile | TornTail | Corrupted
```

EOF, torn-tail, and corruption states are idempotent: later calls report the
same terminal result without reading again. Transient I/O failures do not become
permanent format states.

The `WAL` facade used by `Engine` combines `WALWriter` with batch recovery.

## Engine recovery and rotation

At startup, `Engine` recovers the WAL id selected by the manifest. It accepts a
torn final logical record but rejects corruption. Complete records are applied
to the MemTable and advance the next sequence counter.

Rather than appending to the recovered file, the engine creates generation
`id + 1`, rewrites the recovered complete records, synchronizes the replacement,
and commits the new WAL id to the manifest. Only then does it remove the old
file. This produces a canonical append target without retaining a torn tail.

During a normal flush, the engine may create an empty replacement WAL and add
its id to the same manifest edit that publishes the new L0 SSTable. The old WAL
is deleted after the edit commits and the flushed generation is retired.

## Durability policy

The ordering guarantee is always WAL append before MemTable insertion. The
strength of a successful individual write depends on
`DBOptions::wal.sync_on_write`:

- `true`: synchronize the WAL before applying the MemTable record;
- `false` (default): do not request a per-write sync; flush, rotation, or close
  synchronizes later.

The WAL header is always synchronized when a generation is created.

## Current format limits

- Version 1 uses a fixed 4096-byte block size from `config.h`.
- Key and value lengths are encoded as `u32`.
- Fragment payload length is encoded as `u32`, but a fragment is also bounded
  by one physical block.
- Format fields are little-endian and checksummed with CRC32.
- `start_seq` is stored and checksummed, but the current loader does not enforce
  a globally contiguous or monotonic sequence stream from that value.
- WAL records are individual mutations; there is no transaction or write-batch
  grouping format.
- The configured `DBOptions::block_size` does not replace the version-1 WAL
  constant.

## Source

- Interface: [`include/wal.h`](../../include/wal.h)
- Implementation: [`src/wal.cpp`](../../src/wal.cpp)
- Format constants: [`include/config.h`](../../include/config.h)
- Tests: [`tests/wal_test.cpp`](../../tests/wal_test.cpp)
