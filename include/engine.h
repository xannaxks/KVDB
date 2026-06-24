#pragma once
#include "kvdb.h"
#include "memory"
#include "manifest.h"
#include "wal.h"
#include "mem_table.h"
#include "status.h"

class Engine : KVDB
{
private:
	DBoptions options;

	std::unique_ptr<Manifest> manifest;
	std::unique_ptr<WAL> wal;

	std::unique_ptr<MemTable> mutable_mem_table;
	std::vector<std::unique_ptr<MemTable>> immutable_mem_table;

	std::unique_ptr<CompactionScheduler> compaction_scheduler;

	std::uint64_t last_seq = 0;
	bool closed_ = false;
};