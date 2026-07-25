#pragma once

#include "arena.h"
#include "endian_io.h"
#include "type.h"

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>

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

    [[nodiscard]] std::uint32_t disk_size() const noexcept;
    InternalRecord() = default;
    InternalRecord(InternalRecord&& other) noexcept = default;
    InternalRecord(const InternalRecord& other) noexcept = default;
    InternalRecord(ArenaEntry key_entry, ArenaEntry value_entry, Type type, uint64_t seq_num);

    bool write(std::ofstream& file) const;

    bool operator==(const InternalRecord& other) const noexcept;
    InternalRecord& operator=(const InternalRecord&) noexcept;
    InternalRecord& operator=(InternalRecord&&) noexcept;

	static std::optional<InternalRecord> read(std::ifstream& file, Arena& arena);

    //std::vector<std::byte> return_byte_sequence();
};

//struct ByteRecord
//{
//    ArenaEntry key, value;
//    Type type;
//
//    ByteRecord(const InternalRecord& entry);
//    ByteRecord(ArenaEntry key_entry, ArenaEntry value_entry, Type type);
//};
