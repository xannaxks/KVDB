#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <shared_mutex>
#include <vector>

#include "red_black_tree.h"

class MemTable
{
public:
    struct ImmutableSnapshot
    {
        std::uint64_t generation_id = 0;
        std::shared_ptr<const RBTree> table;
    };

    MemTable();
    ~MemTable() = default;

    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;
    MemTable(MemTable&&) = delete;
    MemTable& operator=(MemTable&&) = delete;

    // Inserts an already materialized internal record into the active table.
    // ArenaEntry storage referenced by the record must remain alive for as long
    // as the corresponding mutable/immutable generation remains readable.
    [[nodiscard]] Status apply(const InternalRecord& entry);

    [[nodiscard]] Status put(
        ArenaEntry key,
        ArenaEntry value,
        std::uint64_t sequence_number
    );

    [[nodiscard]] Status remove(
        ArenaEntry key,
        std::uint64_t sequence_number
    );

    // Returns the newest InternalRecord, including tombstones. A tombstone is a
    // successful lookup and must stop the Engine from searching older SSTables.
    [[nodiscard]] Result<std::optional<InternalRecord>> get(const ArenaEntry& key) const;

    // Moves the current active tree into the immutable queue and installs a new
    // active tree. Freezing an empty active tree is a successful no-op.
    [[nodiscard]] Status freeze_mutable();
    [[nodiscard]] Status manual_freeze();

    // A flush worker keeps this shared snapshot alive while building an SSTable.
    // The table remains visible to reads until retire_oldest_immutable succeeds.
    [[nodiscard]] Result<ImmutableSnapshot> oldest_immutable() const;

    // Retires exactly the generation that was flushed. Returns false when the
    // queue changed or the supplied generation is stale.
    [[nodiscard]] bool retire_oldest_immutable(
        std::uint64_t generation_id
    );

    // Compatibility helper for builders that currently consume a vector.
    // On success, out is replaced with the oldest immutable table's records.
    [[nodiscard]] Status dump_oldest_immutable(
        std::vector<InternalRecord>& out,
        std::uint64_t& generation_id
    ) const;

    [[nodiscard]] bool has_immutable() const;
    [[nodiscard]] std::size_t immutable_count() const;
    [[nodiscard]] std::size_t mutable_memory_usage() const;
    [[nodiscard]] std::size_t approximate_memory_usage() const;

private:
    struct ImmutableTable
    {
        std::uint64_t generation_id = 0;
        std::shared_ptr<const RBTree> table;
    };

    //mutable std::shared_mutex mutex_;
    std::shared_ptr<RBTree> mutable_table_;
    std::deque<ImmutableTable> immutable_tables_;
    std::uint64_t next_generation_id_ = 1;
};