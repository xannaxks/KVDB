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

    InternalRecord() = default;
    InternalRecord(ArenaEntry key_entry, ArenaEntry value_entry, Type type, uint64_t seq_num);
};

struct ByteRecord
{
    ArenaEntry key, value;
    Type type;

    ByteRecord(const InternalRecord& entry);
    ByteRecord(ArenaEntry key_entry, ArenaEntry value_entry, Type type);
};