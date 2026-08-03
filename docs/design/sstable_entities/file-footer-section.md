# File Footer Section

The file footer is the fixed-size section directory at the end of every
[SSTable](../sstable.md). A reader finds it by seeking backward from EOF, then
uses its offsets and sizes to locate the data, index, Bloom, and metadata
sections without scanning the file.

## Purpose

The footer provides two kinds of protection:

- **discovery:** absolute offsets and logical sizes for variable sections;
- **structural validation:** section ordering, alignment, non-overlap, data
  extent, total file size, version, and CRC.

Each variable section still validates its own type, payload, and checksum. The
footer proves that those sections occupy a coherent physical file layout.

## Position in the SSTable

```text
... | Meta section | padding | File footer (76 bytes) | EOF
                                  ^
                             block aligned
```

The footer starts at a 4096-byte boundary and is the final encoded object. The
file ends immediately after its 76 bytes; the footer is not padded to a full
block.

## Disk layout

All fields are little-endian and serialized explicitly:

| Offset | Size | Field | Meaning |
| ---: | ---: | --- | --- |
| 0 | 4 | `magic` | `FILE_FOOTER_MAGIC` (`0x46545231`) |
| 4 | 4 | `version` | SSTable format version |
| 8 | 4 | `reserved` | Must be zero |
| 12 | 8 | `data_offset` | First data block or empty-section cursor |
| 20 | 8 | `data_block_count` | Number of reserved 4096-byte blocks |
| 28 | 8 | `index_offset` | Index section start |
| 36 | 4 | `index_size` | Logical index bytes |
| 40 | 8 | `bloom_offset` | Bloom section start |
| 48 | 4 | `bloom_size` | Logical Bloom bytes |
| 52 | 8 | `meta_offset` | Metadata section start |
| 60 | 4 | `meta_size` | Logical metadata bytes |
| 64 | 8 | `file_size` | Exact final file length |
| 72 | 4 | `footer_crc32` | CRC of every preceding footer field |

The stable encoded size is 76 bytes. It is not `sizeof(FileFooterSection)`,
which may include C++ padding.

## Section directory model

For a non-empty data section, the footer requires:

```text
data_offset % 4096 == 0
index_offset = data_offset + data_block_count * 4096
```

Index, Bloom, metadata, and footer offsets must all be block-aligned and
monotonically increasing:

```text
index_offset < bloom_offset < meta_offset < footer_offset
```

Logical size may be smaller than the physical span to the next aligned section:

```text
index_size <= bloom_offset - index_offset
bloom_size <= meta_offset - bloom_offset
meta_size  <= footer_offset - meta_offset
```

This permits alignment padding but prevents section overlap.

## Finalization and write path

`SSTable::write()` fills the section offsets and sizes after writing data,
index, Bloom, and metadata, then aligns the file cursor for the footer.

`FileFooterSection::finalize(file, footer_offset)` requires that the cursor and
current file size both equal the block-aligned footer offset. It stages:

```text
magic = FILE_FOOTER_MAGIC
version = LATEST_SSTABLE_VERSION
reserved = 0
file_size = footer_offset + 76
footer_crc32 = CRC(all preceding fields)
```

The staged footer is fully validated before it replaces the in-memory object.

`write(file, offset)` repeats finalization, writes all fields, and commits the
staged object only after complete success. Physical bytes and the running offset
cannot be rolled back after an I/O failure.

## Load path

The default loader locates the footer with:

```text
footer_offset = actual_file_size - 76
```

It requires that offset to be block-aligned and that the 76-byte object ends
exactly at EOF. It reads all fields, verifies the footer CRC, and validates the
complete directory against the actual file size.

The optional `file_footer_backwards_offset` parameter supports lower-level
tests or alternate positioning, but a valid footer must still be the final
object. On any error the caller-visible offset is restored.

## Validation

A valid version-1 footer requires:

- correct magic and a supported non-zero version;
- zero reserved field;
- block-aligned footer, index, Bloom, and metadata offsets;
- block-aligned data offset when the data section is non-empty;
- exact data-block extent ending at the index;
- index size at least its 9-byte header;
- Bloom size exactly 153 bytes;
- metadata size in `[65, 4096]`;
- each logical section fitting before the next section;
- `file_size == actual_file_size == footer_offset + 76`;
- matching footer CRC.

For an empty data section, `data_offset` may be an unaligned logical cursor but
may not be after `index_offset`.

## Rebuild helpers

Overloaded `rebuild()` methods can copy derived information from the in-memory
sections:

```text
DataSection  -> data_offset, data_block_count
IndexSection -> index_offset, index_size
BloomSection -> bloom_offset, bloom_size
MetaSection  -> meta_offset, meta_size
```

They check alignment and safe conversion to the footer's `u32` size fields.
`SSTable::write()` currently assigns the same derived values directly and uses
`finalize()` for the complete cross-section check.

## Recovery role

The footer is loaded before variable SSTable sections. `SSTable::load()` uses it
to prove high-level ordering, discover lazy data-block headers, and seek to the
index, Bloom, and metadata. Each loader's consumed size is compared with the
footer-declared extent.

A valid footer does not make corrupt payload data valid; it only makes section
locations bounded and trustworthy enough to attempt their independent
validation.

## Current limits

- The footer format is fixed at version 1 and 76 bytes.
- Section sizes are `u32`; offsets, block count, and file size are `u64`.
- Bloom section size is fixed to the version-1 153-byte representation.
- The footer contains one of each variable section; optional or repeated
  sections are not supported.
- There is no whole-file checksum. Integrity is composed from header, footer,
  and per-section/per-data-block CRCs.

## Source

- Interface: [`include/sstable_entities/file_footer_section.h`](../../../include/sstable_entities/file_footer_section.h)
- Implementation: [`src/sstable_entities/file_footer_section.cpp`](../../../src/sstable_entities/file_footer_section.cpp)
- Tests: [`tests/sstable_entities/file_footer_section_test.cpp`](../../../tests/sstable_entities/file_footer_section_test.cpp)
