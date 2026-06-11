#include "merge_iterator.h"

Status MergeIterator::build(std::vector<SSTableIterator>& data) // data should outlive merge iterator
{
    inputs_ = &data;
    valid_ = false;
    status_ = Status::ok();

    HeapCompare compare{};
    compare.inputs = inputs_;

    heap_ = std::priority_queue<
        HeapItem,
        std::vector<HeapItem>,
        HeapCompare
    >(compare);

    for (std::size_t i = 0; i < data.size(); ++i)
    {
        status_ = data[i].seek_to_first();
        if (!status_.is_ok()) {
            valid_ = false;
            return status_;
        }

        if (data[i].valid()) {
            heap_.push(HeapItem{ i });
        }
    }

    valid_ = !heap_.empty();
    return Status::ok();
}

Status MergeIterator::next()
{
    if (!valid_)
        return Status::ok();

    HeapItem top = heap_.top();
    heap_.pop();

    SSTableIterator& source_it = (*inputs_)[top.iterator_index];

    status_ = source_it.next();
    if (!status_.is_ok()) {
        valid_ = false;
        return status_;
    }

    if (source_it.valid()) {
        heap_.push(top);
    }

    valid_ = !heap_.empty();
    return Status::ok();
}

bool MergeIterator::valid() const
{
    return valid_;
}

Status MergeIterator::status() const
{
    return status_;
}

const InternalRecord& MergeIterator::record() const
{
    assert(valid_);
    assert(!heap_.empty());
    assert(inputs_ != nullptr);

    const HeapItem& top = heap_.top();
    return (*inputs_)[top.iterator_index].record();
}