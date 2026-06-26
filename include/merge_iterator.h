#include "sstable.h"
#include "arena.h"
#include "status.h"
#include "record.h"
#include "table_meta.h"
#include <queue>
#include <vector>
#include "sstable_iterator.h"

static bool internal_before(const InternalRecord& a, const InternalRecord& b)
{
    if (a.key_entry < b.key_entry) return true;
    if (b.key_entry < a.key_entry) return false;

    // Same user key: newer sequence comes first.
    return a.seq_num > b.seq_num;
}
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

    Status build(std::vector<SSTableIterator>& data);
    Status next();

    bool valid() const;
    const InternalRecord& record() const;
    Status status() const;
};