
std::filesystem::path SSTableManager::make_table_path(
    const std::filesystem::path& dir,
    std::uint32_t table_id
) {
    //table_id++;
    return dir / std::format("table-{:09}.sst", table_id);
}

std::filesystem::path SSTableManager::make_tmp_table_path(
    const std::filesystem::path& dir,
    std::uint32_t table_id
) {
    //table_id++;
    return dir / std::format("table-{:09}.sst.tmp", table_id);
}

std::vector<Status> SSTableManager::load(Arena& arena, const std::filesystem::path& root_path)
{
    std::vector<Status> load_results;

    for (std::uint32_t table_id = 1; ; table_id++)
    {
        auto path = make_table_path(root_path, table_id);

        if (!std::filesystem::exists(path))
            break;

        auto sstable = sstable_loader.load(path, arena);

        if (!sstable.is_ok())
        {
            load_results.push_back(std::move(sstable.status));
            continue;
        }

        immutable_pool.emplace_back(std::move(sstable.value));
        load_results.push_back(std::move(sstable.status));
    }

    return load_results;
}

Status SSTableManager::write_latest(bool erase)
{
    if (this->immutable_pool.empty()) return Status::ok();

    Status write_result = this->sstable_writer.write(this->immutable_pool.front());
    if (!write_result.is_ok()) return write_result;

    if (!erase) return Status::ok();

    this->immutable_pool.erase(this->immutable_pool.begin());

    return Status::ok();
}

std::vector<Status> SSTableManager::write_all()
{
    std::vector<Status> result;

    for (auto& sstable : this->immutable_pool)
        result.push_back(this->sstable_writer.write(sstable));

    return result;
}

SSTableManager::SSTableManager()
    : sstable_writer(), sstable_loader()
{
}


/// Adding one more sstable to sstable manager pool


void SSTableManager::add_to_pool(SSTable&& sstable)
{
    this->immutable_pool.emplace_back(std::move(sstable));
}
