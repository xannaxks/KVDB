#pragma once

#include "kvdb.h"
#include "manifest.h"
#include "wal.h"
#include "mem_table.h"
#include "sstable_manager.h"
#include "status.h"
#include "db_options.h"
#include "compaction_scheduler.h"
#include "level_manager.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <cstdint>

class Engine final : public KVDB
{
public:
    explicit Engine(DBOptions options);
    ~Engine() override = default;

    Status open();

    Status put(std::string_view key, std::string_view value) override;
    Result<std::optional<std::string>> get(std::string_view key) override;
    Status remove(std::string_view key) override;

    Status flush() override;

    Status compact_range(
        std::string_view begin,
        std::string_view end
    ) override;

    Status close() override;

private:
    Status ensure_open() const;

    Status recover_manifest();
    Status recover_wal();

    Status maybe_flush();
    Status maybe_compact();

    std::uint32_t allocate_next_table_id();
    std::uint64_t allocate_next_sequence();

private:
    DBOptions options_;

    std::unique_ptr<Manifest> manifest_;
    std::unique_ptr<WAL> wal_;
    std::unique_ptr<MemTable> mem_table_;

    std::unique_ptr<CompactionScheduler> compaction_scheduler_;
    std::unique_ptr<SSTableManager> sstable_manager_;
    std::unique_ptr<LevelManager> level_manager_;

    std::uint64_t last_seq_num_ = 0;
    std::uint32_t last_table_id_ = 1;

    bool opened_ = false;
    bool closed_ = false;
};