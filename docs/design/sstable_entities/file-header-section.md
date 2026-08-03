# File Header Section

The file header is the fixed identity record at the beginning of an
[SSTable](../sstable.md). It lets a reader reject the wrong file type,
unsupported format version, unsupported physical block size, or corrupted
header before interpreting any variable section offsets.

## Purpose

The header records:

- the SSTable magic number;
- format version and flags;
- the physical block size expected by the reader;
- the table id assigned by the manifest;
- a CRC32 protecting those fields.

It does not locate the other sections. That responsibility belongs to the
[file footer](./file-footer-section.md).

## Position in the SSTable

In a normal KVDB SSTable the header starts at byte zero:

```text
offset 0
+-------------------------+
| File header (24 bytes)  |
+-------------------------+
| 0xCD alignment padding  |
+=========================+ offset 4096
| first data block        |
```

The standalone `write()` and `load()` functions align their supplied cursor to
a block boundary, so they can also be tested or embedded from a non-zero
cursor. `SSTable::write()` always begins at zero.

## Disk layout

The encoded header is six little-endian `u32` values, for a stable 24-byte
size:

| Offset | Size | Field | Version 1 meaning |
| ---: | ---: | --- | --- |
| 0 | 4 | `magic` | `FILE_HEADER_MAGIC` (`0x53535431`) |
| 4 | 4 | `version` | `1` |
| 8 | 4 | `flags` | Feature flags; currently defaults to zero |
| 12 | 4 | `block_size` | `4096` |
| 16 | 4 | `table_id` | SSTable identity |
| 20 | 4 | `crc32` | CRC of the first five fields |

The format is serialized field by field and never uses `sizeof(struct)`, so C++
padding cannot affect disk bytes.

Equivalent structure:

```cpp
struct FileHeaderSection
{
    std::uint32_t magic = FILE_HEADER_MAGIC;
    std::uint32_t version = LATEST_SSTABLE_VERSION;
    std::uint32_t flags = 0;
    std::uint32_t block_size = BLOCK_SIZE;
    std::uint32_t table_id = 0;
    std::uint32_t crc32 = 0;
};
```

## CRC

CRC input is the canonical representation of:

```text
magic | version | flags | block_size | table_id
```

The `crc32` field itself is excluded. `write()` refreshes a stale cached CRC
before emitting bytes.

## Write path

`write(file, offset)`:

1. validates magic, supported version, and block size before any I/O;
2. recomputes CRC;
3. verifies/aligned the explicit cursor;
4. writes the six little-endian values within one physical block;
5. verifies the final file cursor and the 24-byte encoded width.

If a physical write fails, the offset continues to describe the bytes already
written; it is not reset to pretend the append was atomic.

## Load path

`load(file, offset)` aligns to a block boundary and reads exactly six `u32`
values. It validates in this order:

```text
magic
CRC
supported version
supported block size
```

Checking CRC before interpreting mutable format fields means a flipped version
bit is reported as corruption rather than mistaken for a genuine future
format. On any failure the caller-visible offset is restored.

## Validation

The current reader accepts a header only when:

- `magic == FILE_HEADER_MAGIC`;
- `1 <= version <= LATEST_SSTABLE_VERSION`;
- `block_size == 4096`;
- the CRC matches.

The header-layer validator does not require a non-zero table id or zero flags.
Production engine publication reserves table id zero at the level/manifest
layer, and version 1 writers currently leave flags at zero.

## Current limits

- File format version is fixed at 1.
- Physical block size is a compile-time SSTable constant, not the configurable
  `DBOptions::block_size` value.
- Table ids in the SSTable layer are 32-bit even though manifest metadata uses
  a 64-bit field.

## Source

- Interface: [`include/sstable_entities/file_header_section.h`](../../../include/sstable_entities/file_header_section.h)
- Implementation: [`src/sstable_entities/file_header_section.cpp`](../../../src/sstable_entities/file_header_section.cpp)
- Tests: [`tests/sstable_entities/file_header_section_test.cpp`](../../../tests/sstable_entities/file_header_section_test.cpp)
