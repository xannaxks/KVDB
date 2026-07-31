/**
 * @file merge_iterator.h
 * @brief K-way merge cursor over sorted SSTable iterators.
 */
#pragma once

#include "sstable.h"
#include "arena.h"
#include "status.h"
#include "record.h"
#include "table_meta.h"
#include <queue>
#include <vector>
#include "sstable_iterator.h"

/** @brief Internal-key ordering: user key ascending, sequence descending. */
static bool internal_before(const InternalRecord& a, const InternalRecord& b)
{
    if (a.key_entry < b.key_entry) return true;
    if (b.key_entry < a.key_entry) return false;

    // Same user key: newer sequence comes first.
    return a.seq_num > b.seq_num;
}
/**
 * @brief Heap-based merge view over multiple SSTableIterator inputs.
 *
 * The iterator exposes the globally smallest internal key. Equal user keys are
 * ordered newest-first, which lets compaction apply its version-retention
 * policy in one forward pass.
 *
 * @note Input iterators must outlive this object.
 */
class MergeIterator
{
private:
    struct HeapItem {
        std::size_t iterator_index;
    };

    struct HeapCompare {
        const std::vector<SSTableIterator>* inputs = nullptr;

        bool operator()(const HeapItem& a, const HeapItem& b) const {
            const auto& ra = (*inputs)[a.iterator_index].record();
            const auto& rb = (*inputs)[b.iterator_index].record();

            // priority_queue puts "highest priority" first.
            // Return true if a should come AFTER b.
            return internal_before(rb, ra);
        }
    };

    std::vector<SSTableIterator>* inputs_ = nullptr;

    HeapCompare compare_{};
    std::priority_queue<HeapItem, std::vector<HeapItem>, HeapCompare> heap_;

    bool valid_ = false;
    Status status_ = Status::ok();

public:
    MergeIterator() noexcept;

    /** @brief Initializes the heap from all valid input iterators. */
    Status build(std::vector<SSTableIterator>& data);
    /** @brief Advances the current input and restores heap order. */
    Status next();

    bool valid() const;
    const InternalRecord& record() const;
    Status status() const;
};
