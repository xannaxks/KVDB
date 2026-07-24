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

class SSTableBuilder
{
public:
    [[nodiscard]] static Result<std::optional<SSTable>> build(
        std::uint32_t table_id,
        MemTable& mem_table,
        const std::filesystem::path& path,
        const std::filesystem::path& final_path
    );

    [[nodiscard]] static Result<std::optional<SSTable>> build(
        std::uint32_t table_id,
        SSTableIterator& it,
        const std::filesystem::path& path,
        const std::filesystem::path& final_path
    );

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

class SSTableStreamingBuilder
{
public:
    SSTableStreamingBuilder(
        std::filesystem::path path,
        std::filesystem::path final_path
    );

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] Result<std::optional<TableMeta>> finish(
        std::uint32_t level,
        Arena& arena
    );
    [[nodiscard]] Status add(const InternalRecord& record);
    [[nodiscard]] std::size_t approximate_disk_space() const;

private:
    SSTable sstable;
};
