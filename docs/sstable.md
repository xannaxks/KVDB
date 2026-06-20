# SSTable

An SSTable, or **Sorted String Table**, is an immutable file format used to store ordered key-value records on disk. It is one of the core components of an [LSM-tree](https://en.wikipedia.org/wiki/Log-structured_merge-tree) based storage engine.

SSTables are optimized for fast reads, sequential writes, and efficient compaction. Once written, an SSTable is never modified in place. Any updates or deletions are represented as newer records in newer SSTables.

## Overview

General information about SSTables and LSM-trees can be found [here](https://medium.com/the-developers-diary/sstables-and-lsm-trees-2e4b6c8be251).

Examples of production SSTable-like formats, such as RocksDB SST files, can be found [here](https://github.com/facebook/rocksdb/wiki/A-Tutorial-of-RocksDB-SST-formats).

This document focuses only on the SSTable implementation used in this project.

## Architecture

Each SSTable is split into several sections. Every section has its own responsibility and is documented separately.

The main sections are:

* [File header section](./sstable_entities/file-header-section.md) — stores basic file metadata, format version, flags, block size, table id, and checksum information.
* [Data section](./sstable_entities/data-section.md) — stores the actual sorted records.
* [Index section](./sstable_entities/index-section.md) — stores key ranges and offsets pointing into the data section.
* [Bloom section](./sstable_entities/bloom-section.md) — stores a Bloom filter for quickly checking whether a key may exist in the table.
* [Meta section](./sstable_entities/meta-section.md) — stores table-level metadata such as record count, tombstone count, and key ranges.
* [File footer section](./sstable_entities/file-footer-section.md) — stores offsets and sizes of other sections, allowing the SSTable to be loaded efficiently.

Records inside the SSTable are sorted by:

```txt
key ASC, seq_num DESC
```

This means keys are ordered lexicographically, and records with the same key are ordered from newest to oldest version.

## File layout

The high-level SSTable layout looks like this:

```txt
+=============================+
|     File Header Section     |
+=============================+

+=============================+
|         Data Section        |
+=============================+
| Data Block Header           |
+-----------------------------+
| Data Block Payload          |
+-----------------------------+
| Data Block Header           |
+-----------------------------+
| Data Block Payload          |
+-----------------------------+
| ...                         |
+=============================+

+=============================+
|        Index Section        |
+=============================+

+=============================+
|        Bloom Section        |
+=============================+

+=============================+
|         Meta Section        |
+=============================+

+=============================+
|     File Footer Section     |
+=============================+
```

The footer is written at the end of the file because it contains the disk offsets of the important sections. During loading, the footer allows the SSTable to locate the index, Bloom filter, metadata, and data section without scanning the entire file.

## Write flow

SSTables are written sequentially. Each section handles its own serialization, while the `SSTable` object coordinates the full write process.

Numeric values are written using little-endian encoding.

The simplified write flow is:

```txt
open temporary SSTable file

offset = 0

write file header

write data section
    while writing data blocks:
        collect index entries

rebuild bloom section from data section
rebuild meta section from data and index sections

write index section
write bloom section
write meta section

fill footer with:
    data offset
    data block count
    index offset and size
    bloom offset and size
    meta offset and size

align file to block boundary

finalize footer
write footer

flush file to disk
close file
rename temporary file to final SSTable path
sync parent directory
```

In pseudocode:

```txt
function write_sstable():
    file = open_writable_file(temp_path)
    offset = 0

    file_header.write(file, offset)

    data_offset = data_section.write(file, offset, index_section)

    bloom_section.rebuild(data_section)
    meta_section.rebuild(data_section, index_section)

    index_offset = index_section.write(file, offset)
    bloom_offset = bloom_section.write(file, offset)
    meta_offset = meta_section.write(file, offset)

    footer.data_offset = data_offset
    footer.data_block_count = data_section.block_count()

    footer.index_offset = index_offset
    footer.index_size = index_section.disk_size()

    footer.bloom_offset = bloom_offset
    footer.bloom_size = bloom_section.disk_size()

    footer.meta_offset = meta_offset
    footer.meta_size = meta_section.disk_size()

    align_to_block_boundary(file, offset)

    footer.finalize(file, offset)
    footer.write(file, offset)

    file.sync()
    file.close()

    durable_rename(temp_path, final_path)
    sync_parent_directory()
```

The file is first written to a temporary path. After the write succeeds and the file is flushed, it is renamed to its final path. This prevents partially written SSTables from appearing as valid tables.

## Load flow

SSTable loading is lazy. The table does not load all data records into memory immediately.

Instead, only the required metadata and lookup structures are loaded:

* file header
* file footer
* data section view
* index section
* Bloom section
* meta section

The actual data blocks are loaded only when they are needed during iteration or search.

Simplified load flow:

```txt
open SSTable file

load file header
load file footer

using offsets from footer:
    load data section view
    load index section
    load bloom section
    load meta section

return loaded SSTable object
```

In pseudocode:

```txt
function load_sstable(path):
    file = open_readable_file(path)
    offset = 0

    file_header = load_file_header(file, offset)
    footer = load_file_footer(file)

    data_view = load_data_section_view(
        file,
        footer.data_offset,
        footer.data_block_count
    )

    index = load_index_section(file, footer.index_offset)
    bloom = load_bloom_section(file, footer.bloom_offset)
    meta = load_meta_section(file, footer.meta_offset)

    return SSTable(
        file_header,
        data_view,
        index,
        bloom,
        meta,
        footer
    )
```

This approach keeps memory usage low, especially when many SSTables exist at the same time.

## Search flow

When the user requests a key, the SSTable uses several layers of filtering before reading the actual data block.

The search process is:

```txt
check Bloom filter

if Bloom filter says key is definitely not present:
    return NotFound

use index section to find candidate data block

if no matching key range exists:
    return NotFound

load candidate data block

binary search inside the data block

if key is not found:
    return NotFound

if newest record for key is a tombstone:
    return Deleted or NotFound

return value
```

In pseudocode:

```txt
function get(key):
    if bloom.may_contain(key) == false:
        return NotFound

    block_info = index.find_candidate_block(key)

    if block_info does not exist:
        return NotFound

    block = data_view.load_block(block_info.offset)

    record = block.binary_search(key)

    if record does not exist:
        return NotFound

    if record.type == Tombstone:
        return Deleted

    return record.value
```

The Bloom filter avoids unnecessary disk reads for keys that definitely do not exist in the SSTable. The index section narrows the search to one candidate data block. The data block is then searched directly.

## Atomicity and durability

SSTable writing follows a temporary-file strategy:

```txt
write *.sst.tmp
sync file
close file
rename *.sst.tmp -> *.sst
sync parent directory
```

This protects the database from incomplete SSTables caused by crashes or process termination during writing.

The final SSTable path is only published after the file is fully written and synced.

## Notes

This document only describes the high-level SSTable structure and flow.

Detailed formats of individual sections are described in their own documentation files:

* `file-header-section.md`
* `data-section.md`
* `index-section.md`
* `bloom-section.md`
* `meta-section.md`
* `file-footer-section.md`
