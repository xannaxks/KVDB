/**
 * @file engine.h
 * @brief Coordinates recovery, reads, writes, flushing, and compaction.
 */
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

/**
 * @brief Monotonic maintenance counters for one Engine instance.
 *
 * A flush is counted after an immutable MemTable generation is published and
 * retired. A compaction is counted after its VersionEdit is committed. Empty
 * flush requests and compaction attempts that produce no edit are not counted.
 */
struct EngineStatistics
{
    std::uint64_t flush_count = 0;
    std::uint64_t compaction_count = 0;
};

/**
 * @brief Concrete LSM-tree implementation of the KVDB interface.
 *
 * Engine owns the WAL, mutable/immutable MemTables, manifest, level metadata,
 * SSTable manager, and compaction scheduler. Public operations are serialized
 * by an internal mutex so that WAL ordering, sequence assignment, and metadata
 * publication remain consistent.
 *
 * The write path is WAL -> MemTable -> optional flush/compaction. The read path
 * checks the MemTable generations first, then searches candidate SSTables from
 * newest to oldest.
 */
class Engine final : public KVDB
{
public:
    explicit Engine(DBOptions options);
    ~Engine() override;

    /**
     * @brief Creates or recovers all persistent and in-memory engine state.
     * @return Status::ok() after the manifest and WAL are ready for appends.
     * @callgraph
     */
    Status open();

    /** @copydoc KVDB::put */
    /** @callgraph */
    Status put(std::string& key, std::string& value) override;
    /** @copydoc KVDB::get */
    /** @callgraph */
    Result<std::optional<std::string>> get(std::string_view key) override;
    /** @copydoc KVDB::remove */
    Status remove(std::string& key) override;
    /** @copydoc KVDB::flush */
    /** @callgraph */
    Status flush() override;

    /** @copydoc KVDB::compact_range */
    /** @callgraph */
    Status compact_range(
        std::string_view begin,
        std::string_view end
    ) override;

    /** @copydoc KVDB::close */
    Status close() override;

    /** @brief Returns a consistent snapshot of maintenance counters. */
    [[nodiscard]] EngineStatistics statistics() const;

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

    EngineStatistics statistics_{};

    bool opened_ = false;
    bool closed_ = false;

    mutable std::mutex mutex_;
};
