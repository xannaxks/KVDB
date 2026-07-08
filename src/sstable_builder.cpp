#include "sstable_builder.h"

Result<std::optional<SSTable>> SSTableBuilder::build(
    std::uint32_t table_id,
    MemTable& mem_table,
    std::filesystem::path& path,
    std::filesystem::path& final_path
)
{
    std::vector<InternalRecord> records;
    mem_table.dump_oldest_immutable(records);

    return build_impl(table_id, records, path, final_path);
}

Result<std::optional<SSTable>> SSTableBuilder::build(
    std::uint32_t table_id,
    SSTableIterator& it,
    std::filesystem::path& path,
    std::filesystem::path& final_path
)
{
    std::vector<InternalRecord> records;

    Status status = it.seek_to_first();
    if (!status.is_ok()) {
        return Result<std::optional<SSTable>>::fail(std::move(status));
    }

    while (it.valid()) {
        records.emplace_back(it.record());

        status = it.next();
        if (!status.is_ok()) {
            return Result<std::optional<SSTable>>::fail(std::move(status));
        }
    }

    return build_impl(table_id, records, path, final_path);
}

Result<std::optional<SSTable>> SSTableBuilder::build_impl(
    std::uint32_t table_id,
    const std::vector<InternalRecord>& records,
    std::filesystem::path& path,
    std::filesystem::path& final_path
)
{
    if (records.empty()) {
        return Result<std::optional<SSTable>>::ok(std::nullopt);
    }

    SSTable sstable(path, final_path);

    for (const auto& record : records) {
        Status status = sstable.append_record(record);
        if (!status.is_ok()) {
            return Result<std::optional<SSTable>>::fail(std::move(status));
        }
    }

    //Status status = sstable.finish();
    //if (!status.is_ok()) {
    //    return Result<std::optional<SSTable>>::fail(std::move(status));
    //}

    return Result<std::optional<SSTable>>::ok(std::move(sstable));
}