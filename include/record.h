#pragma once

#include "type.h"
#include "bytes.h"

struct Record
{
    std::string key, value;
    Type type;
};

struct InternalRecord
{
    Bytes key, value;
    Type type;
    uint64_t seq_num;

    InternalRecord() = default;
    InternalRecord(const std::string& key, const std::string& value, Type type, uint64_t seq_num);
    InternalRecord(const Bytes& key, const Bytes& value, Type type, uint64_t seq_num);
};

struct ByteRecord
{
    Bytes key, value;
    Type type;

    ByteRecord(const InternalRecord& entry);
    ByteRecord(const std::string& key, const std::string& value, Type type);
    ByteRecord(const Bytes& key, const Bytes& value, Type type);
};