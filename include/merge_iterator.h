#include "sstable.h"
#include "arena.h"
#include "status.h"
#include "record.h"
#include "table_meta.h"
#include <queue>
#include <vector>

class MergeIterator
{
private:
	std::priority_queue<InternalRecord> heap;
 
	bool valid_ = false;
	Status status_ = Status::ok();

public:
	Status build(std::vector<SSTableIterator>& data);
	Status next();

	bool valid() const;
	const InternalRecord& record();
	Status status() const;
};