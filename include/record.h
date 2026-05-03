#pragma once

#include "type.h"
#include "bytes.h"
#include "arena.h"

struct Record
{
    std::string key, value;
    Type type;
};

struct InternalRecord
{
    ArenaEntry key_entry, value_entry;
    Type type;
    uint64_t seq_num;

    uint32_t disk_size();
    InternalRecord() = default;
    InternalRecord(ArenaEntry key_entry, ArenaEntry value_entry, Type type, uint64_t seq_num);

    std::vector<std::byte> return_byte_sequence();
};

struct ByteRecord
{
    ArenaEntry key, value;
    Type type;

    ByteRecord(const InternalRecord& entry);
    ByteRecord(ArenaEntry key_entry, ArenaEntry value_entry, Type type);
};