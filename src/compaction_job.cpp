#include "compaction_job.h"

#include "sstable_iterator.h"
#include "file.h"
#include "merge_iterator.h"

Result<std::optional<VersionEdit>> CompactionJob::run(
    const CompactionPlan& plan,
    LevelManager& level_manager,
    SSTableManager& sstable_manager,
    Arena& arena
)
{
    if (!plan.validate()) {
        return Result<std::optional<VersionEdit>>::fail(
            Status{
                StatusCode::Corruption,
                "Compaction plan is not valid"
            }
        );
    }

    VersionEdit edit;
    std::vector<SSTableIterator> iterators;

    /*
        Helper for opening one SSTable and creating an iterator.

        Important:
        We only mark the table as deleted in VersionEdit.
        We do NOT delete it from SSTableManager pool here.
        Actual deletion should happen after Manifest commit.
    */
    auto add_input_table = [&](const TableMeta& table_meta,
        std::size_t level) -> Status
        {
            Result<std::optional<SSTable>> sstable_result =
                sstable_manager.find(table_meta.table_id);

            if (!sstable_result.is_ok()) {
                return sstable_result.status;
            }

            if (!sstable_result.value.has_value()) {
                return Status{
                    StatusCode::NotFound,
                    "SSTable not found during compaction"
                };
            }

            SSTable& sstable = *sstable_result.value;

            Result<std::unique_ptr<ReadableFile>> file_result =
                open_readable_file(sstable.get_final_path());

            if (!file_result.is_ok()) {
                return file_result.status;
            }

            iterators.emplace_back(
                sstable,
                std::move(file_result.value),
                arena
            );

            edit.payload.deleted_tables.emplace_back(
                table_meta.table_id,
                level
            );

            return Status::ok();
        };

    for (const auto& table_meta : plan.source_tables) {
        Status s = add_input_table(table_meta, plan.source_level);
        if (!s.is_ok()) {
            return Result<std::optional<VersionEdit>>::fail(s);
        }
    }

    for (const auto& table_meta : plan.overlapping_tables) {
        Status s = add_input_table(table_meta, plan.target_level);
        if (!s.is_ok()) {
            return Result<std::optional<VersionEdit>>::fail(s);
        }
    }

    if (iterators.empty()) {
        return Result<std::optional<VersionEdit>>::ok(std::nullopt);
    }

    MergeIterator merge_iterator;
    merge_iterator.build(iterators);

    /*
        Output builder state.

        Compaction may produce several output SSTables:

            L1 table_10 [a, f]
            L1 table_11 [g, n]
            L1 table_12 [o, z]

        instead of one huge file.
    */
    std::optional<SSTableBuilder> builder;

    auto start_new_output = [&]() -> Status
        {
            Result<SSTableBuilder> builder_result =
                sstable_manager.create_builder(plan.target_level, arena);

            if (!builder_result.is_ok()) {
                return builder_result.status;
            }

            builder.emplace(std::move(builder_result.value));
            return Status::ok();
        };

    auto finish_current_output = [&]() -> Status
        {
            if (!builder.has_value()) {
                return Status::ok();
            }

            if (builder->empty()) {
                builder.reset();
                return Status::ok();
            }

            Result<TableMeta> meta_result = builder->finish();

            if (!meta_result.is_ok()) {
                return meta_result.status;
            }

            edit.payload.new_tables.push_back(
                std::move(meta_result.value)
            );

            builder.reset();
            return Status::ok();
        };

    Status s = start_new_output();
    if (!s.is_ok()) {
        return Result<std::optional<VersionEdit>>::fail(s);
    }

    const bool is_bottommost_level =
        plan.target_level + 1 >= level_manager.level_count();

    while (merge_iterator.valid()) {
        /*
            MergeIterator must return records in this order:

                user_key ascending
                sequence descending

            So for each user key, the first record is newest.
        */
        const InternalRecord& newest = merge_iterator.record();
        ArenaEntry current_key = newest.key_entry;

        bool keep_record = true;

        /*
            Simplified tombstone rule:

            If newest record is DELETE and we are compacting into the
            bottommost level, we can drop it.

            If not bottommost, keep tombstone because lower levels may still
            contain old values hidden by this delete.
        */
        if (newest.type == ::Type::Tombstone && is_bottommost_level) {
            keep_record = false;
        }

        if (keep_record) {
            Status add_status = builder->add(newest);
            if (!add_status.is_ok()) {
                return Result<std::optional<VersionEdit>>::fail(add_status);
            }
        }

        /*
            Skip all older versions of this same user key.

            Example input:

                apple seq=20 PUT
                apple seq=10 PUT
                apple seq=4  DELETE

            Output keeps only seq=20.
        */
        merge_iterator.next();

        while (merge_iterator.valid() &&
            merge_iterator.record().key_entry == current_key) {
            merge_iterator.next();
        }

        /*
            Split output only after finishing the whole user key.

            This avoids putting different versions of the same user key
            into different output SSTables.
        */
        if (builder.has_value() &&
            !builder->empty() &&
            builder->estimated_size() >= plan.max_output_file_size) {
            Status finish_status = finish_current_output();
            if (!finish_status.is_ok()) {
                return Result<std::optional<VersionEdit>>::fail(finish_status);
            }

            Status start_status = start_new_output();
            if (!start_status.is_ok()) {
                return Result<std::optional<VersionEdit>>::fail(start_status);
            }
        }
    }

    s = finish_current_output();
    if (!s.is_ok()) {
        return Result<std::optional<VersionEdit>>::fail(s);
    }

    /*
        If compaction only removed deleted keys/tombstones and produced
        no output tables, this is still a valid VersionEdit if it deletes
        old tables.

        So return edit as long as something changed.
    */
    if (edit.payload.deleted_tables.empty() &&
        edit.payload.new_tables.empty()) {
        return Result<std::optional<VersionEdit>>::ok(std::nullopt);
    }

    return Result<std::optional<VersionEdit>>::ok(std::move(edit));
}