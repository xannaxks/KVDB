#include "merge_iterator.h"

Status MergeIterator::build(std::vector<SSTableIterator>& data)
{
	valid_ = false;
	status_ = Status::ok();
	for (auto& it : data)
	{
		status_ = it.seek_to_first();
		if (!status_.is_ok())
			return status_;

		while (it.valid())
		{
			this->heap.push(it.record());
			status_ = it.next();
			if (!status_.is_ok())
				return status_;
		}
	}

	valid_ = !this->heap.empty();

	return Status::ok();
}

Status MergeIterator::next()
{
	if (this->heap.empty())
	{
		status_ = Status{ StatusCode::Underflow, "Tried to delete top of empty heap to move iterator" };
		return status_;
	}
	this->heap.pop();
	if (this->heap.empty())
	{
		valid_ = false;
	}
	return Status::ok();
}

bool MergeIterator::valid() const
{
	return this->valid_;
}

Status MergeIterator::status() const
{
	return this->status_;
}

const InternalRecord& MergeIterator::record()
{
	if (this->heap.empty())
	{
		valid_ = false;
		status_ = Status{ StatusCode::BadAccess, "Accessing empty heap" };
		return {};
	}

	return this->heap.top();
}