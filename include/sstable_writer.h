#include "sstable.h"
#include "status.h"
#include "record.h"

class SSTableWriter
{
public:
	SSTableWriter();

	static Status write(SSTable& sstable);
	static Status write(MemTable& mem_table);
	static Status write(std::vector<InternalRecord>& records);
};