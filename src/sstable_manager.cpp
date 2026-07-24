#include "sstable_manager.h"

#include <format>
#include <utility>

std::filesystem::path SSTableManager::make_table_path(
    std::uint32_t table_id,
    const std::filesystem::path& dir
)
{
    return dir / std::format("table-{:09}.sst", table_id);
}

std::filesystem::path SSTableManager::make_tmp_table_path(
    std::uint32_t table_id,
    const std::filesystem::path& dir
)
{
    return dir / std::format("table-{:09}.sst.tmp", table_id);
}

SSTableManager::SSTableManager(std::filesystem::path path)
    : db_dir(std::move(path))
{
}

Result<std::shared_ptr<SSTable>> SSTableManager::get(
    std::uint32_t table_id,
    Arena& arena
)
{
    const auto it = pool.find(table_id);
    if (it != pool.end()) {
        return Result<std::shared_ptr<SSTable>>::ok(it->second);
    }

    return open(table_id, arena);
}

Result<std::shared_ptr<SSTable>> SSTableManager::open(
    std::uint32_t table_id,
    Arena& arena
)
{
    return open_impl(table_id, make_table_path(table_id, db_dir), arena);
}

Result<std::shared_ptr<SSTable>> SSTableManager::open(
    const TableMeta& meta,
    Arena& arena
)
{
    return open_impl(meta.table_id, meta.path, arena);
}

Result<std::shared_ptr<SSTable>> SSTableManager::open_impl(
    std::uint32_t table_id,
    const std::filesystem::path& path,
    Arena& arena
)
{
    const auto cached = pool.find(table_id);
    if (cached != pool.end()) {
        return Result<std::shared_ptr<SSTable>>::ok(cached->second);
    }

    auto sstable_result = SSTableLoader::load(path, arena);
    if (!sstable_result.is_ok()) {
        return Result<std::shared_ptr<SSTable>>::fail(
            std::move(sstable_result.status)
        );
    }

    auto sstable_ptr = std::make_shared<SSTable>(
        std::move(sstable_result.value)
    );

    const auto [it, inserted] = pool.emplace(table_id, sstable_ptr);
    if (!inserted) {
        return Result<std::shared_ptr<SSTable>>::ok(it->second);
    }

    return Result<std::shared_ptr<SSTable>>::ok(std::move(sstable_ptr));
}

Result<std::optional<SSTable>> SSTableManager::build(
    std::uint32_t table_id,
    MemTable& mem_table
)
{
    const std::filesystem::path path = make_tmp_table_path(table_id, db_dir);
    const std::filesystem::path final_path = make_table_path(table_id, db_dir);

    return SSTableBuilder::build(
        table_id,
        mem_table,
        path,
        final_path
    );
}

Result<std::optional<SSTable>> SSTableManager::build(
    std::uint32_t table_id,
    SSTableIterator& iterator
)
{
    const std::filesystem::path path = make_tmp_table_path(table_id, db_dir);
    const std::filesystem::path final_path = make_table_path(table_id, db_dir);

    return SSTableBuilder::build(
        table_id,
        iterator,
        path,
        final_path
    );
}

Status SSTableManager::write(SSTable& sstable)
{
    return SSTableWriter::write(sstable);
}

std::unique_ptr<SSTableStreamingBuilder>
SSTableManager::create_streaming_builder(std::uint32_t table_id)
{
    return std::make_unique<SSTableStreamingBuilder>(
        make_tmp_table_path(table_id, db_dir),
        make_table_path(table_id, db_dir)
    );
}
