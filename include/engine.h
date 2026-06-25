#pragma once
#include "kvdb.h"
#include <memory>
#include "manifest.h"
#include "wal.h"
#include "mem_table.h"
#include "sstable_manager.h"
#include "status.h"
#include "db_options.h"
#include "compaction_scheduler.h"

class Engine final : public KVDB
{
public:
    explicit Engine(DBOptions options);

    Status put(std::string_view key, std::string_view value) override;

    Result<std::optional<std::string>> get(std::string_view key) override;

    Status remove(std::string_view key) override;

    Status flush() override;

    Status compact_range(std::string_view begin,
        std::string_view end) override;

    Status close() override;

private:
    DBOptions options_;

    std::unique_ptr<Manifest> manifest_;
    std::unique_ptr<WAL> wal_;

    std::unique_ptr<MemTable> mutable_mem_table_;

    std::unique_ptr<CompactionScheduler> compaction_scheduler_;
    std::unique_ptr<SSTableManager> sstable_manager_;

    std::uint64_t last_seq_ = 0;
    bool closed_ = false;
};