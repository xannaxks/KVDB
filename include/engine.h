#pragma once
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

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
#include <filesystem>
#include <mutex>

class Engine final : public KVDB
{
public:
    explicit Engine(DBOptions options);
    ~Engine() override;

    Status open();

    Status put(std::string& key, std::string& value) override;
    Result<std::optional<std::string>> get(std::string_view key) override;
    Status remove(std::string& key) override;
    Status flush() override;

    Status compact_range(
        std::string_view begin,
        std::string_view end
    ) override;

    Status close() override;

private:
    Status ensure_open() const;
    Status prepare_dirs();
    Status open_manifest();
    Status open_wal();
    Status put_impl(
        std::string_view key,
        std::string_view value,
        Type type
    );
    Status flush_unlocked();
    Status flush_oldest_immutable();
    Status maybe_flush_unlocked();
    Status maybe_compact_unlocked();
    Status run_compaction(const CompactionPlan& plan);

    [[nodiscard]] std::filesystem::path wal_path(
        std::uint32_t wal_id
    ) const;

private:
    DBOptions options_;

    std::unique_ptr<Manifest> manifest_;
    std::unique_ptr<LevelManager> level_manager_;

    std::unique_ptr<WAL> wal_;
    std::unique_ptr<MemTable> mem_table_;

    std::unique_ptr<CompactionScheduler> compaction_scheduler_;
    std::unique_ptr<SSTableManager> sstable_manager_;

    std::unique_ptr<Arena> arena_;

    std::filesystem::path manifest_path_;
    std::filesystem::path sstable_dir_;

    std::uint64_t next_sequence_ = 1;
    std::uint32_t current_wal_id_ = 1;

    bool opened_ = false;
    bool closed_ = false;

    mutable std::mutex mutex_;
};
