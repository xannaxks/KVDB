#pragma once
#include "arena.h"
#include "status.h"
#include "record.h"
#include "type.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>


struct VirtualNode
{
    ArenaEntry key_entry;
    ArenaEntry value_entry;
    const std::uint64_t seq_number;
    ::Type type;

	VirtualNode(ArenaEntry key_entry, ArenaEntry value_entry, Type type, uint64_t seq_num)
		: key_entry(key_entry), value_entry(value_entry), seq_number(seq_num), type(type)
    {
	}

    virtual bool operator<(const VirtualNode& other) const;
    virtual bool operator>(const VirtualNode& other) const;
    virtual bool operator==(const VirtualNode& other) const;

    virtual bool operator<(const ::InternalRecord& other) const;
    virtual bool operator>(const ::InternalRecord& other) const;
    virtual bool operator==(const ::InternalRecord& other) const;

    virtual std::size_t approximate_memory_usage() const;
};

class VirtualInorderIterator
{
public:
	virtual ~VirtualInorderIterator() = default;
	virtual bool has_next() = 0;
	virtual VirtualNode* next() = 0;
};

class Driver
{
public:
    Driver() = default;
	virtual ~Driver() = default;

    Driver(const Driver&) = delete;
    Driver& operator=(const Driver&) = delete;

    virtual ::Status insert(const InternalRecord& entry) = 0;

    virtual Result<std::optional<InternalRecord>>
        find_latest_by_key(ArenaEntry key) const = 0;

    virtual bool validate() const = 0;

    virtual std::size_t approximate_memory_usage() const = 0;

    virtual bool empty() const noexcept = 0;

    /** @brief Appends all records to @p out in internal-key order. */
    virtual void dump_inorder(std::vector<InternalRecord>& out) const = 0;
};