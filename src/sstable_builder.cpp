#include "sstable_builder.h"
#include "sstable_entities.h"
#include "sstable_entities/index_section.h"

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

Result<std::optional<SSTable>> SSTableBuilder::build(
    std::uint32_t table_id,
    const std::vector<InternalRecord>& records,
    std::filesystem::path& path,
    std::filesystem::path& final_path
)
{
	return build_impl(table_id, records, path, final_path);
}

std::uint64_t SSTableBuilder::approximate_disk_space(
    const std::vector<InternalRecord>& records
)
{
    std::uint64_t records_size = 0;
    std::uint64_t index_size = 0;

    const std::uint64_t block_size = SSTableEntities::BLOCK_SIZE;

    std::uint64_t current_block_size = 0;
    const InternalRecord* last_record_in_block = nullptr;

    for (const auto& record : records) {
        const std::uint64_t record_size = record.disk_size();
        records_size += record_size;

        if (current_block_size > 0 &&
            current_block_size + record_size > block_size)
        {
            // index entry for the block that just ended
            index_size += SSTableEntities::IndexSection::Payload::fixed_disk_size();
            index_size += last_record_in_block->key_entry.size;
            // or internal key size, depending on your format

            current_block_size = 0;
            last_record_in_block = nullptr;
        }

        current_block_size += record_size;
        last_record_in_block = &record;
    }

    // final block index entry
    if (current_block_size > 0 && last_record_in_block != nullptr) {
        index_size += SSTableEntities::IndexSection::Payload::fixed_disk_size();
        index_size += last_record_in_block->key_entry.size;
    }

    return SSTable::fixed_disk_size()
        + SSTableEntities::IndexSection::Header::disk_size()
        + records_size
        + index_size;
}


/// SSTableStreamingBuilder implementation

SSTableStreamingBuilder::SSTableStreamingBuilder(
	std::filesystem::path& path,
	std::filesystem::path& final_path
)
	: sstable(path, final_path)
{
}

bool SSTableStreamingBuilder::empty() const
{
	return this->sstable.get_data_section().data_blocks.empty();
}

Status SSTableStreamingBuilder::add(const InternalRecord& record)
{
	return this->sstable.append_record(record);
}

std::size_t SSTableStreamingBuilder::approximate_disk_space() const
{
	std::vector<InternalRecord> records;
	for (const auto& block : this->sstable.get_data_section().data_blocks) {
		for (const auto& payload : block.payloads) {
			InternalRecord record;
			record.key_entry = { payload.key_ptr, payload.key_size };
			record.value_entry = { payload.value_ptr, payload.value_size };
			record.type = payload.type;
			record.seq_num = payload.seq_num;
			records.push_back(std::move(record));
		}
	}
	return SSTableBuilder::approximate_disk_space(records);
}

Result<std::optional<TableMeta>> SSTableStreamingBuilder::finish(std::uint32_t level, Arena& arena)
{
	// Finalize the SSTable and return its metadata
	Status status = this->sstable.write();
	if (!status.is_ok()) {
		return Result<std::optional<TableMeta>>::fail(std::move(status));
	}
	Result<TableMeta> meta_result = make_table_meta(this->sstable, level, arena);
	if (!meta_result.is_ok()) {
		return Result<std::optional<TableMeta>>::fail(std::move(meta_result.status));
	}
	return Result<std::optional<TableMeta>>::ok(std::move(meta_result.value));
}
