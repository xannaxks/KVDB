/**
 * @file sstable_manager.h
 * @brief SSTable naming, construction, loading, and shared instance caching.
 */
#pragma once

#include "arena.h"
#include "mem_table.h"
#include "sstable.h"
#include "sstable_builder.h"
#include "sstable_iterator.h"
#include "sstable_loader.h"
#include "status.h"
#include "table_meta.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <unordered_map>

/**
 * @brief Coordinates SSTable file lifecycle within one database directory.
 *
 * The manager assigns canonical temporary/final paths, delegates building and
 * loading, and caches loaded tables by id so readers can share immutable
 * structural metadata.
 *
 * @note Cache access is not currently synchronized.
 */
class SSTableManager
{
public:
    explicit SSTableManager(std::filesystem::path db_dir);

    [[nodiscard]] Result<std::optional<SSTable>> build(
        std::uint32_t table_id,
        MemTable& mem_table
    );

    [[nodiscard]] Result<std::optional<SSTable>> build(
        std::uint32_t table_id,
        SSTableIterator& iterator
    );

    /** @brief Loads a table by id without requiring existing TableMeta. */
    [[nodiscard]] Result<std::shared_ptr<SSTable>> open(
        std::uint32_t table_id,
        Arena& arena
    );

    [[nodiscard]] Result<std::shared_ptr<SSTable>> open(
        const TableMeta& meta,
        Arena& arena
    );

    [[nodiscard]] Status write(SSTable& sstable);

    /** @brief Returns a cached table or loads and caches it on demand. */
    [[nodiscard]] Result<std::shared_ptr<SSTable>> get(
        std::uint32_t table_id,
        Arena& arena
    );

    /** @brief Creates a builder with canonical temporary and final paths. */
    [[nodiscard]] std::unique_ptr<SSTableStreamingBuilder>
        create_streaming_builder(std::uint32_t table_id);

private:
    std::filesystem::path db_dir;
    std::unordered_map<std::uint32_t, std::shared_ptr<SSTable>> pool;

    [[nodiscard]] static std::filesystem::path make_table_path(
        std::uint32_t table_id,
        const std::filesystem::path& dir
    );

    [[nodiscard]] static std::filesystem::path make_tmp_table_path(
        std::uint32_t table_id,
        const std::filesystem::path& dir
    );

    [[nodiscard]] Result<std::shared_ptr<SSTable>> open_impl(
        std::uint32_t table_id,
        const std::filesystem::path& path,
        Arena& arena
    );
};
