#include "sstable_builder.h"

#include "sstable_entities/index_section.h"

#include <limits>
#include <utility>

namespace
{
    [[nodiscard]] bool record_less(
        const InternalRecord& lhs,
        const InternalRecord& rhs
    )
    {
        if (lhs.key_entry == rhs.key_entry) {
            return lhs.seq_num > rhs.seq_num;
        }
        return lhs.key_entry < rhs.key_entry;
    }

    [[nodiscard]] Status validate_record(const InternalRecord& record)
    {
        if (record.key_entry.size > 0 && record.key_entry.data == nullptr) {
            return Status{
                StatusCode::InvalidArgument,
                "SSTable record has a non-zero key size with a null key pointer"
            };
        }

        if (record.value_entry.size > 0 && record.value_entry.data == nullptr) {
            return Status{
                StatusCode::InvalidArgument,
                "SSTable record has a non-zero value size with a null value pointer"
            };
        }

        return Status::ok();
    }

    [[nodiscard]] Status validate_records(
        const std::vector<InternalRecord>& records
    )
    {
        for (std::size_t i = 0; i < records.size(); ++i) {
            Status status = validate_record(records[i]);
            if (!status.is_ok()) {
                return status;
            }

            if (i > 0 && record_less(records[i], records[i - 1])) {
                return Status{
                    StatusCode::InvalidArgument,
                    "SSTable input records are not sorted by key ascending and sequence descending"
                };
            }
        }

        return Status::ok();
    }

    [[nodiscard]] std::uint64_t saturating_add(
        std::uint64_t lhs,
        std::uint64_t rhs
    ) noexcept
    {
        const auto max = std::numeric_limits<std::uint64_t>::max();
        if (lhs > max - rhs) {
            return max;
        }
        return lhs + rhs;
    }
}

Result<std::optional<SSTable>> SSTableBuilder::build(
    std::uint32_t table_id,
    MemTable& mem_table,
    const std::filesystem::path& path,
    const std::filesystem::path& final_path
)
{
    std::vector<InternalRecord> records;
    mem_table.dump_oldest_immutable(records);

    return build_impl(table_id, records, path, final_path);
}

Result<std::optional<SSTable>> SSTableBuilder::build(
    std::uint32_t table_id,
    SSTableIterator& it,
    const std::filesystem::path& path,
    const std::filesystem::path& final_path
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

Result<std::optional<SSTable>> SSTableBuilder::build(
    std::uint32_t table_id,
    const std::vector<InternalRecord>& records,
    const std::filesystem::path& path,
    const std::filesystem::path& final_path
)
{
    return build_impl(table_id, records, path, final_path);
}

Result<std::optional<SSTable>> SSTableBuilder::build_impl(
    std::uint32_t table_id,
    const std::vector<InternalRecord>& records,
    const std::filesystem::path& path,
    const std::filesystem::path& final_path
)
{
    // table_id is encoded by SSTableManager into path/final_path. Keep the
    // parameter for API compatibility until/if the on-disk header stores it.
    (void)table_id;

    if (records.empty()) {
        return Result<std::optional<SSTable>>::ok(std::nullopt);
    }

    Status validation = validate_records(records);
    if (!validation.is_ok()) {
        return Result<std::optional<SSTable>>::fail(std::move(validation));
    }

    SSTable sstable(path, final_path);

    for (const auto& record : records) {
        Status status = sstable.append_record(record);
        if (!status.is_ok()) {
            return Result<std::optional<SSTable>>::fail(std::move(status));
        }
    }

    return Result<std::optional<SSTable>>::ok(
        std::optional<SSTable>{ std::move(sstable) }
    );
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
        records_size = saturating_add(records_size, record_size);

        // Match DataSection's normal block-splitting rule as closely as
        // possible. add_payload() remains authoritative for oversized records.
        if (current_block_size > 0 &&
            record_size <= std::numeric_limits<std::uint64_t>::max() - current_block_size &&
            current_block_size + record_size > block_size)
        {
            index_size = saturating_add(
                index_size,
                SSTableEntities::IndexSection::Payload::fixed_disk_size()
            );
            index_size = saturating_add(
                index_size,
                last_record_in_block->key_entry.size
            );

            current_block_size = 0;
            last_record_in_block = nullptr;
        }

        current_block_size = saturating_add(current_block_size, record_size);
        last_record_in_block = &record;
    }

    if (current_block_size > 0 && last_record_in_block != nullptr) {
        index_size = saturating_add(
            index_size,
            SSTableEntities::IndexSection::Payload::fixed_disk_size()
        );
        index_size = saturating_add(
            index_size,
            last_record_in_block->key_entry.size
        );
    }

    std::uint64_t total = SSTable::fixed_disk_size();
    total = saturating_add(
        total,
        SSTableEntities::IndexSection::Header::disk_size()
    );
    total = saturating_add(total, records_size);
    total = saturating_add(total, index_size);
    return total;
}

SSTableStreamingBuilder::SSTableStreamingBuilder(
    std::filesystem::path path,
    std::filesystem::path final_path
)
    : sstable(std::move(path), std::move(final_path))
{
}

bool SSTableStreamingBuilder::empty() const noexcept
{
    return sstable.get_data_section().data_blocks.empty();
}

Status SSTableStreamingBuilder::add(const InternalRecord& record)
{
    Status validation = validate_record(record);
    if (!validation.is_ok()) {
        return validation;
    }

    const auto& blocks = sstable.get_data_section().data_blocks;
    if (!blocks.empty() && !blocks.back().payloads.empty()) {
        const auto& payload = blocks.back().payloads.back();

        InternalRecord previous{};
        previous.key_entry = { payload.key_ptr, payload.key_size };
        previous.value_entry = { payload.value_ptr, payload.value_size };
        previous.type = payload.type;
        previous.seq_num = payload.seq_num;

        if (record_less(record, previous)) {
            return Status{
                StatusCode::InvalidArgument,
                "SSTable streaming input is not sorted by key ascending and sequence descending"
            };
        }
    }

    return sstable.append_record(record);
}

std::size_t SSTableStreamingBuilder::approximate_disk_space() const
{
    std::vector<InternalRecord> records;

    for (const auto& block : sstable.get_data_section().data_blocks) {
        for (const auto& payload : block.payloads) {
            InternalRecord record;
            record.key_entry = { payload.key_ptr, payload.key_size };
            record.value_entry = { payload.value_ptr, payload.value_size };
            record.type = payload.type;
            record.seq_num = payload.seq_num;
            records.push_back(std::move(record));
        }
    }

    const std::uint64_t size = SSTableBuilder::approximate_disk_space(records);
    const auto max_size_t = std::numeric_limits<std::size_t>::max();
    if (size > max_size_t) {
        return max_size_t;
    }
    return static_cast<std::size_t>(size);
}

Result<std::optional<TableMeta>> SSTableStreamingBuilder::finish(
    std::uint32_t level,
    Arena& arena
)
{
    if (empty()) {
        return Result<std::optional<TableMeta>>::ok(std::nullopt);
    }

    Status status = sstable.write();
    if (!status.is_ok()) {
        return Result<std::optional<TableMeta>>::fail(std::move(status));
    }

    Result<TableMeta> meta_result = make_table_meta(sstable, level, arena);
    if (!meta_result.is_ok()) {
        return Result<std::optional<TableMeta>>::fail(
            std::move(meta_result.status)
        );
    }

    return Result<std::optional<TableMeta>>::ok(
        std::optional<TableMeta>{ std::move(meta_result.value) }
    );
}
