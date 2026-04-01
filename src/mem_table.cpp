#include "mem_table.h"
#include <cassert>
#include <new>
#include <utility>

MemTable::MemTable()
    : mutable_table(std::make_unique<RBTree>())
{
}

MemTable::~MemTable() = default;

MemTable::Status MemTable::status_from(RBTree::Status rb_status)
{
    switch (rb_status)
    {
    case RBTree::Status::OK:
        return MemTable::Status::OK;
    case RBTree::Status::KeyNotFound:
        return MemTable::Status::KeyNotFound;
    case RBTree::Status::KeyWasDeleted:
        return MemTable::Status::KeyWasDeleted;
    case RBTree::Status::MemoryAllocationFailed:
        return MemTable::Status::MemoryAllocationFailed;
    }

    assert(false);
    return MemTable::Status::MemoryAllocationFailed;
}

std::variant<InternalRecord, MemTable::Status> MemTable::read_latest(const Bytes& key) const
{
    std::variant<InternalRecord, RBTree::Status> result = this->mutable_table->find_latest_by_key(key);

    if (std::holds_alternative<InternalRecord>(result))
        return std::get<InternalRecord>(result);

    for (auto it = this->immutable_tables.rbegin(); it != this->immutable_tables.rend(); ++it)
    {
        result = (*it)->find_latest_by_key(key);

        if (std::holds_alternative<InternalRecord>(result))
            return std::get<InternalRecord>(result);
        if (std::get<RBTree::Status>(result) != RBTree::Status::KeyNotFound)
            return MemTable::status_from(std::get<RBTree::Status>(result));
    }

    return Status::KeyNotFound;
}

MemTable::Status MemTable::freeze_mutable()
{
    try
    {
        std::unique_ptr<RBTree> new_table = std::make_unique<RBTree>();
        this->immutable_tables.push_back(std::move(this->mutable_table));
        this->mutable_table = std::move(new_table);
        return Status::OK;
    }
    catch (const std::bad_alloc&)
    {
        return Status::MemoryAllocationFailed;
    }
}

MemTable::Status MemTable::manual_freeze()
{
    return this->freeze_mutable();
}

MemTable::Status MemTable::apply(const InternalRecord& entry)
{
    return 
        MemTable::status_from(
            this->mutable_table->insert(entry)
        );
}

MemTable::Status MemTable::put(const Bytes& key, const Bytes& value, uint64_t seq_num)
{
    return apply(InternalRecord(key, value, Type::Put, seq_num));
}

MemTable::Status MemTable::remove(const Bytes& key, uint64_t seq_num)
{
    return apply(InternalRecord(key, Bytes(), Type::Tombstone, seq_num));
}

std::variant<ByteRecord, MemTable::Status> MemTable::get(const Bytes& key) const
{
    std::variant<InternalRecord, Status> result = this->read_latest(key);
    if (std::holds_alternative<InternalRecord>(result))
    {
        const InternalRecord& record = std::get<InternalRecord>(result);
		if (record.type == Type::Tombstone)
            return MemTable::Status::KeyWasDeleted;
        return ByteRecord(record);
    }
	return std::get<MemTable::Status>(result);
}

void MemTable::dump_oldest_immutable(std::vector<InternalRecord>& out)
{
    if (this->immutable_tables.empty())
        return;

    immutable_tables.front()->dump_inorder(out);
}
void MemTable::drop_oldest_immutable()
{
    if (!this->immutable_tables.empty())
        this->immutable_tables.erase(this->immutable_tables.begin());
}

bool MemTable::has_immutable() const
{
    return !this->immutable_tables.empty();
}
