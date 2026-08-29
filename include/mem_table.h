/**
 * @file mem_table.h
 * @brief Mutable and immutable in-memory generations for recent records.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <shared_mutex>
#include <vector>
#include <functional>

#include "driver.h"


// @brief Factory function type for creating new Driver instances.
using DriverFactory = std::function<std::shared_ptr<Driver>()>;

/**
 * @brief Thread-safe owner of the active RBTree and its immutable generations.
 *
 * New records enter the mutable tree. Freezing atomically moves that tree to
 * an ordered immutable queue and installs a fresh mutable tree. Readers inspect
 * all generations, while flush code holds a shared snapshot until the exact
 * generation is durably published and retired.
 *
 * @note The ArenaEntry storage referenced by records is externally owned and
 *       must outlive every generation that can still be read.
 */
class MemTable
{
public:
    /** @brief Shared handle that pins one immutable generation during a flush. */
    struct ImmutableSnapshot
    {
        std::uint64_t generation_id = 0;
        std::shared_ptr<const Driver> table;
    };

    MemTable() = default;
    MemTable(DriverFactory driver_factory);
    ~MemTable() = default;

    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;
    MemTable(MemTable&&) = delete;
    MemTable& operator=(MemTable&&) = delete;

    /**
     * @brief Inserts an already materialized record into the active tree.
     * @param entry Versioned record whose byte ranges remain externally owned.
     */
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

    /**
     * @brief Finds the newest in-memory version of @p key.
     * @return The newest InternalRecord, including a tombstone, or an empty
     *         optional when no generation contains the key.
     *
     * A returned tombstone is a successful lookup and must prevent callers from
     * searching older SSTables.
     */
    [[nodiscard]] Result<std::optional<InternalRecord>> get(const ArenaEntry& key) const;

    /**
     * @brief Moves the active tree to the immutable queue.
     *
     * A fresh active tree is installed atomically. Freezing an empty active
     * tree is a successful no-op.
     */
    [[nodiscard]] Status freeze_mutable();
    [[nodiscard]] Status manual_freeze();

    /**
     * @brief Pins and returns the oldest generation waiting to be flushed.
     *
     * The generation remains visible to reads until
     * retire_oldest_immutable() succeeds.
     */
    [[nodiscard]] Result<ImmutableSnapshot> oldest_immutable() const;

    /**
     * @brief Retires exactly the flushed generation.
     * @return false when the queue changed or @p generation_id is stale.
     */
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
        std::shared_ptr<const Driver> table;
    };

    DriverFactory driver_factory;

    mutable std::shared_mutex mutex_;
    std::shared_ptr<Driver> mutable_table_;
    std::deque<ImmutableTable> immutable_tables_;
    std::uint64_t next_generation_id_ = 1;
};
