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

    [[nodiscard]] Result<std::shared_ptr<SSTable>> open(
        std::uint32_t table_id,
        Arena& arena
    );

    [[nodiscard]] Result<std::shared_ptr<SSTable>> open(
        const TableMeta& meta,
        Arena& arena
    );

    [[nodiscard]] Status write(SSTable& sstable);

    [[nodiscard]] Result<std::shared_ptr<SSTable>> get(
        std::uint32_t table_id,
        Arena& arena
    );

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
