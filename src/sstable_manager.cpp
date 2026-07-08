#include "sstable_manager.h"
#include <filesystem>
#include <format>

std::filesystem::path SSTableManager::make_table_path(
    std::uint32_t table_id,
    const std::filesystem::path& dir
) {
    //table_id++;
    return dir / std::format("table-{:09}.sst", table_id);
}

std::filesystem::path SSTableManager::make_tmp_table_path(
    std::uint32_t table_id,
    const std::filesystem::path& dir
) {
    //table_id++;
    return dir / std::format("table-{:09}.sst.tmp", table_id);
}

Result<std::shared_ptr<SSTable>> SSTableManager::get(std::uint32_t table_id, Arena& arena)
{
    auto it = this->pool.find(table_id);
    if (it != this->pool.end()) {
        return Result<std::shared_ptr<SSTable>>::ok(it->second);
    }

    return this->open(table_id, arena);
}

Result<std::shared_ptr<SSTable>> SSTableManager::open(std::uint32_t table_id, Arena& arena)
{
    return this->open_impl(
        table_id,
        make_table_path(table_id, this->db_dir),
        arena
    );
}

Result<std::shared_ptr<SSTable>> SSTableManager::open( TableMeta& meta, Arena& arena)
{
    return this->open_impl(
        meta.table_id,
        meta.path,
        arena
    );
}

Result<std::shared_ptr<SSTable>> SSTableManager::open_impl(
    std::uint32_t table_id,
     std::filesystem::path& path,
    Arena& arena
)
{
	return this->open_impl(
		table_id,
		std::move(path),
		arena
	);
}

Result<std::shared_ptr<SSTable>> SSTableManager::open_impl(
    std::uint32_t table_id,
    std::filesystem::path&& path,
    Arena& arena
)
{
    auto it = this->pool.find(table_id);
    if (it != this->pool.end()) {
        return Result<std::shared_ptr<SSTable>>::ok(it->second);
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

    this->pool.emplace(table_id, sstable_ptr);

    return Result<std::shared_ptr<SSTable>>::ok(sstable_ptr);
}

Result<std::optional<SSTable>> SSTableManager::build(
    std::uint32_t table_id,
     MemTable& mem_table,
    Arena& arena
)
{
	std::filesystem::path path = SSTableManager::make_tmp_table_path(table_id, this->db_dir);
	std::filesystem::path final_path = SSTableManager::make_table_path(table_id, this->db_dir);

	return SSTableBuilder::build(table_id, mem_table, path, final_path);
}

Result<std::optional<SSTable>> SSTableManager::build(
    std::uint32_t table_id,
     SSTableIterator& iterator,
    Arena& arena
)
{
    std::filesystem::path path = SSTableManager::make_tmp_table_path(table_id, this->db_dir);
    std::filesystem::path final_path = SSTableManager::make_table_path(table_id, this->db_dir);

    return SSTableBuilder::build(table_id, iterator, path, final_path);
}

Status SSTableManager::write(SSTable& sstable)
{
    return sstable.write();
}

SSTableManager::SSTableManager(std::filesystem::path& path)
    : db_dir(path)
{}

SSTableManager::SSTableManager(std::filesystem::path&& path)
    : db_dir(path)
{}

//std::vector<Status> SSTableManager::load(Arena& arena, const std::filesystem::path& root_path)
//{
//    std::vector<Status> load_results;
//
//    for (std::uint32_t table_id = 1; ; table_id++)
//    {
//        auto path = make_table_path(root_path, table_id);
//
//        if (!std::filesystem::exists(path))
//            break;
//
//        auto sstable = sstable_loader->load(path, arena);
//
//        if (!sstable.is_ok())
//        {
//            load_results.push_back(std::move(sstable.status));
//            continue;
//        }
//
//        pool.emplace_back(std::move(sstable.value));
//        load_results.push_back(std::move(sstable.status));
//    }
//
//    return load_results;
//}
//
//Status SSTableManager::write_latest(bool erase)
//{
//    if (this->unflushed.empty()) return Status::ok();
//
//    Status write_result = this->sstable_writer.write(this->immutable_pool.front());
//    if (!write_result.is_ok()) return write_result;
//
//    if (!erase) return Status::ok();
//
//    this->immutable_pool.erase(this->immutable_pool.begin());
//
//    return Status::ok();
//}
//
//std::vector<Status> SSTableManager::write_all()
//{
//    std::vector<Status> result;
//
//    for (auto& sstable : this->immutable_pool)
//        result.push_back(this->sstable_writer.write(sstable));
//
//    return result;
//}
//
//SSTableManager::SSTableManager()
//    : sstable_writer(), sstable_loader()
//{
//}
//
//
///// Adding one more sstable to sstable manager pool
//
//
//void SSTableManager::add_to_pool(SSTable&& sstable)
//{
//    this->immutable_pool.emplace_back(std::move(sstable));
//}

//Result<std::optional<TableMeta>> SSTableManager::create_from_memtable(
//    std::uint32_t table_id,
//    const MemTable& mem_table,
//    const SSTableBuilderOptions& builder_options,
//    Arena& arena
//)
//{
//    std::filesystem::path path = SSTableManager::make_tmp_table_path(table_id, this->db_dir);
//    std::filesystem::path final_path = SSTableManager::make_table_path(table_id, this->db_dir);
//    SSTableBuilder builder(builder_options);
//    SSTable sstable = std::move(builder.build_from_memtable(mem_table));
//
//    Result<TableMeta> result_table_meta = make_table_meta(sstable, 0, arena);
//    if (!result_table_meta.is_ok())
//        return Result<std::optional<TableMeta>>::fail(std::move(result_table_meta.status));
//
//    SSTableWriter writer(path, final_path);
//    Status write_result = writer.write_and_flush(sstable);
//    if (!write_result.is_ok())
//        return Result<std::optional<TableMeta>>::fail(std::move(write_result));
//
//    return Result<std::optional<TableMeta>>::ok(std::move(result_table_meta.value));
//}
//
//Result<std::optional<TableMeta>> SSTableManager::create_from_iterator(
//    std::uint32_t table_id,
//    const SSTableI& mem_table,
//    const SSTableBuilderOptions& builder_options,
//    Arena& arena
//)
//{
//    std::filesystem::path path = SSTableManager::make_tmp_table_path(table_id, this->db_dir);
//    std::filesystem::path final_path = SSTableManager::make_table_path(table_id, this->db_dir);
//    SSTableBuilder builder(builder_options);
//    SSTable sstable = std::move(builder.build_from_memtable(mem_table));
//
//    Result<TableMeta> result_table_meta = make_table_meta(sstable, 0, arena);
//    if (!result_table_meta.is_ok())
//        return Result<std::optional<TableMeta>>::fail(std::move(result_table_meta.status));
//
//    SSTableWriter writer(path, final_path);
//    Status write_result = writer.write_and_flush(sstable);
//    if (!write_result.is_ok())
//        return Result<std::optional<TableMeta>>::fail(std::move(write_result));
//
//    return Result<std::optional<TableMeta>>::ok(std::move(result_table_meta.value));
//}