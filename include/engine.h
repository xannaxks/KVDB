#pragma once

#include "kvdb.h"
#include "manifest.h"
#include "wal.h"
#include "mem_table.h"
#include "sstable_manager.h"
#include "status.h"
#include "arena.h"
#include "db_options.h"
#include "compaction_scheduler.h"
#include "level_manager.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <cstdint>

class Engine
{
public:
    explicit Engine(DBOptions options);
    ~Engine() = default;

    Status open();

    Status put(std::string& key, std::string& value) ;
    Result<std::optional<std::string>> get(std::string_view key) ;
    Status remove(std::string_view key) ;

    Status recover();

    Status flush() ;

    Status compact_range(
        std::string_view begin,
        std::string_view end
    ) ;

    Status close() ;

private:
    Status ensure_open() const;

    Status prepare_dirs();

    Status recover_manifest();
    Status recover_wal();
    Status recover_sstables();
    Status recover_counters();

    Status maybe_flush();
    Status maybe_compact();

    Result<std::uint32_t> allocate_next_table_id();
    Result<std::uint64_t> allocate_next_sequence();

private:
    DBOptions options_;

    std::unique_ptr<Manifest> manifest_;
    std::unique_ptr<LevelManager> level_manager_;

    std::unique_ptr<WAL> wal_;
    std::unique_ptr<MemTable> mem_table_;

    std::unique_ptr<CompactionScheduler> compaction_scheduler_;
    std::unique_ptr<SSTableManager> sstable_manager_;

    std::unique_ptr<Arena> arena;

    std::uint64_t last_seq_num_ = 0;
    std::uint32_t last_table_id_ = 1;

    bool opened_ = false;
    bool closed_ = false;
};