# Manifest

[The Manifest](https://github.com/facebook/rocksdb/wiki/MANIFEST) stores durable metadata changes for the database.

It does not store user key-value records directly. Instead, it stores changes to the database structure, such as:

* newly created SSTables
* deleted SSTables
* next table id
* next sequence number
* current WAL id

During recovery, the Manifest is loaded and replayed from the beginning. Each stored edit is applied to rebuild the current in-memory metadata state.

---

## Purpose

The Manifest acts as an append-only metadata log.

Instead of rewriting the whole database state every time an SSTable is created or deleted, the engine appends a small `VersionEdit`.

```text
Manifest file:

+-----------------+
| Manifest Header |
+-----------------+
| VersionEdit #1  |
+-----------------+
| VersionEdit #2  |
+-----------------+
| VersionEdit #3  |
+-----------------+
| ...             |
+-----------------+
```

After replaying all valid edits, the engine reconstructs the latest known state of the database.

---

## Manifest Header

The Manifest starts with a fixed-size header.

```cpp
ManifestHeader {
    magic
    version
    header_size
    flags
    reserved
    crc32
}
```

The header is used to verify that the file is really a Manifest file and that the format version is supported.

```text
load_manifest_header(file):
    read magic
    if magic != MANIFEST_MAGIC:
        return BadMagic

    read version
    if version != MANIFEST_VERSION:
        return UnsupportedVersion

    read header_size
    if header_size != expected_header_size:
        return InvalidHeader

    read flags
    read reserved
    read crc32

    compute expected_crc32

    if expected_crc32 != crc32:
        return ChecksumMismatch

    return header
```

---

## VersionEdit

A `VersionEdit` represents one metadata update.

For example, one edit can describe:

* remove old SSTables after compaction
* add a newly compacted SSTable
* update the next table id
* update the next sequence number
* update the current WAL id

```cpp
VersionEdit {
    header
    payload
}
```

Each edit has a small header and a variable-size payload.

```cpp
VersionEditHeader {
    crc32
    payload_size
}
```

```cpp
VersionEditPayload {
    optional next_table_id
    optional next_sequence_number
    optional current_wal_id

    deleted_tables[]
    new_tables[]
}
```

---

## Deleted Table

A deleted table entry identifies which SSTable should be removed from the in-memory metadata state.

```cpp
DeletedTable {
    level
    table_id
}
```

It does not delete the file by itself. It only records that the table should no longer be part of the active database version.

```text
write_deleted_table(table):
    write table.level
    write table.table_id
```

```text
load_deleted_table(file):
    table.level = read_u32()
    table.table_id = read_u64()

    return table
```

---

## VersionEdit Payload Format

The payload starts with flags.

Flags describe which optional fields are present.

```text
flags:
    HAS_NEXT_TABLE_ID
    HAS_NEXT_SEQUENCE_NUMBER
    HAS_CURRENT_WAL_ID
```

After the flags, the payload stores optional fields, table counts, deleted tables, and new tables.

```text
VersionEdit Payload:

+-----------------------+
| flags                 |
+-----------------------+
| next_table_id?        |
+-----------------------+
| next_sequence_number? |
+-----------------------+
| current_wal_id?       |
+-----------------------+
| deleted_table_count   |
+-----------------------+
| new_table_count       |
+-----------------------+
| deleted_tables[]      |
+-----------------------+
| new_tables[]          |
+-----------------------+
```

Pseudo-code:

```text
write_payload(payload):
    flags = calculate_payload_flags(payload)

    write flags

    if payload has next_table_id:
        write next_table_id

    if payload has next_sequence_number:
        write next_sequence_number

    if payload has current_wal_id:
        write current_wal_id

    write deleted_table_count
    write new_table_count

    for table in deleted_tables:
        write_deleted_table(table)

    for table in new_tables:
        write_table_meta(table)
```

---

## Loading a VersionEdit Payload

When loading a payload, the Manifest checks that only known flags are used.

This prevents newer or corrupted Manifest formats from being interpreted incorrectly.

```text
load_payload(file, payload_size):
    payload_begin = current_offset
    payload_end = payload_begin + payload_size

    flags = read_u32()

    if flags contain unknown bits:
        return InvalidPayloadSize

    if HAS_NEXT_TABLE_ID:
        payload.next_table_id = read_u64()

    if HAS_NEXT_SEQUENCE_NUMBER:
        payload.next_sequence_number = read_u64()

    if HAS_CURRENT_WAL_ID:
        payload.current_wal_id = read_u64()

    deleted_count = read_u32()
    new_count = read_u32()

    for i in 0 .. deleted_count:
        payload.deleted_tables.push(load_deleted_table(file))

    for i in 0 .. new_count:
        payload.new_tables.push(load_table_meta(file))

    if current_offset != payload_end:
        return InvalidPayloadSize

    return payload
```

The final offset check is important. It ensures that the payload consumed exactly the number of bytes specified by the `VersionEdit` header.

---

## VersionEdit Checksum

Each `VersionEdit` stores a CRC32 checksum for its payload.

The checksum is computed over:

* flags
* optional fields
* deleted table count
* new table count
* deleted tables
* new table metadata

```text
write_version_edit(edit):
    edit.header.payload_size = edit.payload.disk_size()
    edit.header.crc32 = compute_crc32(edit.payload)

    write edit.header
    write edit.payload
```

When loading:

```text
load_version_edit(file):
    edit_begin = current_offset

    header = load_version_edit_header(file)
    payload = load_payload(file, header.payload_size)

    actual_crc32 = compute_crc32(payload)

    if actual_crc32 != header.crc32:
        return ChecksumMismatch

    return VersionEdit(header, payload)
```

This allows the Manifest to detect corrupted or partially written edits.

---

## Creating a New Manifest

When the database is created, a new Manifest file is initialized with only the Manifest header.

```text
open_or_create_manifest(path):
    if path is empty:
        return InvalidArgument

    if file already exists and is not empty:
        return AlreadyExists

    open file for writing

    header = ManifestHeader()
    header.crc32 = compute_crc32(header)

    write header
    sync file

    return OK
```

The Manifest starts with:

```text
next_table_id = 1
current_wal_id = 1
next_sequence_number = 1
```

---

## Loading and Replaying Manifest

During recovery, the database loads the Manifest header first, then replays every valid `VersionEdit`.

```text
load_manifest(path):
    open file for reading
    file_size = get_file_size()

    offset = 0

    header = load_manifest_header(file)

    manifest.header = header
    manifest.append_offset = offset

    while offset < file_size:
        edit_begin = offset

        edit = load_version_edit(file)

        if edit failed:
            if error is safe trailing error:
                manifest.append_offset = edit_begin
                stop replay
            else:
                return error

        apply(edit)

        manifest.append_offset = offset

    check_invariants()

    return manifest
```

Some trailing errors can be ignored during recovery:

```text
UnexpectedEOF
ChecksumMismatch
InvalidPayloadSize
Corruption
```

This is useful when the process crashes while appending a new edit. Fully written and valid edits are replayed, while incomplete trailing data is ignored.

---

## Applying a VersionEdit

Applying an edit updates the in-memory metadata state.

```text
apply(edit):
    max_table_id_seen = 0

    for deleted in edit.deleted_tables:
        level_manager.remove_table(deleted.level, deleted.table_id)
        max_table_id_seen = max(max_table_id_seen, deleted.table_id)

    for table in edit.new_tables:
        level_manager.add_table(table)
        max_table_id_seen = max(max_table_id_seen, table.table_id)

    if edit has next_table_id:
        next_table_id = edit.next_table_id
    else if max_table_id_seen >= next_table_id:
        next_table_id = max_table_id_seen + 1

    if edit has next_sequence_number:
        next_sequence_number = edit.next_sequence_number

    if edit has current_wal_id:
        current_wal_id = edit.current_wal_id

    check_invariants()

    return OK
```

The Manifest does not directly manage SSTable contents. It only updates metadata describing which SSTables belong to the current database version.

---

## Commit Flow

A commit appends a `VersionEdit`, syncs it to disk, and only then applies it to memory.

```text
commit(edit):
    append(edit)

    sync(manifest_file)

    apply(edit)

    return OK
```

This order matters.

The edit is made durable before the in-memory state is updated. If the process crashes after sync, the edit can be replayed during recovery.

---

## Appending a VersionEdit

Appending requires the Manifest to be opened for writing.

The tracked append offset must match the writable file's current position.

```text
append(edit):
    if manifest is not open for writing:
        return FailedPrecondition

    if writable.current_position != append_offset:
        return InvalidOffset

    write_version_edit(edit)

    return OK
```

The offset check protects against accidentally writing at the wrong location.

---

## Table ID Allocation

The Manifest owns the next SSTable id.

```text
allocate_table_id():
    id = next_table_id
    next_table_id += 1

    return id
```

After recovery, `next_table_id` must always be greater than every live table id.

---

## Sequence Number Tracking

The Manifest stores the next sequence number, not the last one.

Therefore:

```text
last_sequence_number():
    if next_sequence_number == 0:
        return 0

    return next_sequence_number - 1
```

This keeps allocation logic simple because the next usable value is stored directly.

---

## Invariants

After loading or applying edits, the Manifest checks basic consistency rules.

```text
check_invariants():
    if next_table_id == 0:
        return InvariantViolation

    if current_wal_id == 0:
        return InvariantViolation

    for each level in level_manager:
        tables = level_manager.get_tables(level)

        if tables == null:
            return InvariantViolation

        for table in tables:
            if table.level != level:
                return InvariantViolation

            if table.table_id >= next_table_id:
                return InvariantViolation

            if table.largest_key < table.smallest_key:
                return InvariantViolation

    return OK
```

These checks make sure the recovered metadata state is valid before the database continues running.

---

## Summary

The Manifest is responsible for durable metadata recovery.

Its main responsibilities are:

* store metadata edits in append-only form
* verify file headers and edit checksums
* replay edits during recovery
* rebuild the active `LevelManager` state
* track `next_table_id`
* track `next_sequence_number`
* track `current_wal_id`

The Manifest does not store user data directly. It only stores enough metadata to recover which SSTables are active and how future ids should be allocated.

More information can be found [here](http://github.com/facebook/rocksdb/wiki/MANIFEST) and [here](https://en.wikipedia.org/wiki/Manifest_file)