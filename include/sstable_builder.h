/**
 * @file sstable_builder.h
 * @brief Batch and streaming construction of sorted immutable tables.
 */
#pragma once

#include "arena.h"
#include "mem_table.h"
#include "record.h"
#include "sstable.h"
#include "sstable_iterator.h"
#include "status.h"
#include "table_meta.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

/**
 * @brief Converts an immutable record source into a publishable SSTable.
 *
 * The overloads normalize MemTable, iterator, and vector inputs into the same
 * ordered build path. An empty source returns an empty optional instead of
 * creating a file.
 */
class SSTableBuilder
{
public:
    /** @brief Builds from the oldest immutable MemTable generation. */
    [[nodiscard]] static Result<std::optional<SSTable>> build(
        std::uint32_t table_id,
        MemTable& mem_table,
        const std::filesystem::path& path,
        const std::filesystem::path& final_path
    );

    /** @brief Builds from the remaining records of @p it. */
    [[nodiscard]] static Result<std::optional<SSTable>> build(
        std::uint32_t table_id,
        SSTableIterator& it,
        const std::filesystem::path& path,
        const std::filesystem::path& final_path
    );

    /**
     * @brief Builds from records already sorted in internal-key order.
     * @callgraph
     */
    [[nodiscard]] static Result<std::optional<SSTable>> build(
        std::uint32_t table_id,
        const std::vector<InternalRecord>& records,
        const std::filesystem::path& path,
        const std::filesystem::path& final_path
    );

    [[nodiscard]] static std::uint64_t approximate_disk_space(
        const std::vector<InternalRecord>& records
    );

private:
    [[nodiscard]] static Result<std::optional<SSTable>> build_impl(
        std::uint32_t table_id,
        const std::vector<InternalRecord>& records,
        const std::filesystem::path& path,
        const std::filesystem::path& final_path
    );
};

/**
 * @brief Incremental SSTable builder used by compaction output rolling.
 *
 * add() preserves a single in-progress table. finish() writes and publishes the
 * file, then derives TableMeta for the destination level.
 */
class SSTableStreamingBuilder
{
public:
    SSTableStreamingBuilder(
        std::filesystem::path path,
        std::filesystem::path final_path,
        std::uint32_t table_id = 0
    );

    [[nodiscard]] bool empty() const noexcept;
    /** @brief Publishes the table and returns its manifest metadata. */
    [[nodiscard]] Result<std::optional<TableMeta>> finish(
        std::uint32_t level,
        Arena& arena
    );
    /** @brief Appends one record in internal-key order. */
    [[nodiscard]] Status add(const InternalRecord& record);
    [[nodiscard]] std::size_t approximate_disk_space() const;

private:
    SSTable sstable;
};
