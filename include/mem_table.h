#pragma once

#include <memory>
#include <variant>
#include <vector>
#include "record.h"
#include "red_black_tree.h"
#include <algorithm>
#include <string>
#include <string_view>
#include "status.h"

class SSTableBuilder;

class MemTable
{
public:
    //enum class Status
    //{
    //    OK,
    //    KeyNotFound,
    //    KeyWasDeleted,
    //    MemoryAllocationFailed
    //};

    MemTable();
    ~MemTable();

    static Status status_from(Status rb_status);

    Status manual_freeze();
    Status apply(const InternalRecord& entry);
    Status put(std::string_view key, std::string_view value, uint64_t seq_num);
    Status remove(std::string_view key, uint64_t seq_num);
    Result<std::optional<InternalRecord>> get(std::string_view key) const;

    void dump_oldest_immutable(std::vector<InternalRecord>& out);
    void drop_oldest_immutable();
    bool has_immutable() const;

private:
    std::unique_ptr<RBTree> mutable_table;
    std::vector<std::unique_ptr<RBTree>> immutable_tables;



    std::variant<InternalRecord, Status> read_latest(const Bytes& key) const;
    Status freeze_mutable();

    friend class SSTable;
    friend class SSTLoader;
    friend class SSTWriter;
};