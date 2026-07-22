#include "compaction_job.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "compaction_record_policy.h"
#include "file.h"
#include "merge_iterator.h"
#include "sstable_iterator.h"

namespace
{
    bool level_contains_table(
        const LevelManager& manager,
        std::uint32_t level,
        std::uint64_t table_id
    ) noexcept
    {
        const auto* tables = manager.get_lx_tables(level);
        if (tables == nullptr) {
            return false;
        }

        return std::any_of(
            tables->begin(),
            tables->end(),
            [table_id](const TableMeta& table) {
                return table.table_id == table_id;
            }
        );
    }

    bool plan_is_current(
        const CompactionPlan& plan,
        const LevelManager& manager
    ) noexcept
    {
        for (const auto& table : plan.source_tables) {
            if (!level_contains_table(manager, plan.source_level, table.table_id)) {
                return false;
            }
        }

        for (const auto& table : plan.overlapping_tables) {
            if (!level_contains_table(manager, plan.target_level, table.table_id)) {
                return false;
            }
        }

        return true;
    }

    bool is_bottommost_destination(
        const LevelManager& manager,
        std::uint32_t target_level
    ) noexcept
    {
        if (target_level == std::numeric_limits<std::uint32_t>::max()) {
            return true;
        }

        for (std::uint32_t level = target_level + 1; ; ++level) {
            const auto* tables = manager.get_lx_tables(level);
            if (tables == nullptr) {
                return true;
            }

            if (!tables->empty()) {
                return false;
            }

            if (level == std::numeric_limits<std::uint32_t>::max()) {
                return true;
            }
        }
    }
} // namespace

Result<std::optional<VersionEdit>> CompactionJob::run(
    const CompactionPlan& plan,
    const Manifest& manifest,
    SSTableManager& sstable_manager,
    Arena& arena
) const
{
    if (!plan.validate()) {
        return Result<std::optional<VersionEdit>>::fail(
            Status{
                StatusCode::Corruption,
                "Compaction plan is not valid"
            }
        );
    }

    const LevelManager& level_manager = manifest.level_manager();
    if (!plan_is_current(plan, level_manager)) {
        return Result<std::optional<VersionEdit>>::fail(
            Status{
                StatusCode::InvalidState,
                "Compaction plan is stale; one or more input tables changed"
            }
        );
    }

    VersionEdit edit;
    std::vector<std::shared_ptr<SSTable>> pinned_tables;
    std::vector<SSTableIterator> iterators;

    const std::size_t input_count =
        plan.source_tables.size() + plan.overlapping_tables.size();
    pinned_tables.reserve(input_count);
    iterators.reserve(input_count);

    auto cleanup_finished_outputs = [&]() noexcept {
        for (const auto& table : edit.payload.new_tables) {
            if (table.path.empty()) {
                continue;
            }
            std::error_code ignored;
            std::filesystem::remove(table.path, ignored);
        }
        edit.payload.new_tables.clear();
        };

    auto fail = [&](Status status)
        -> Result<std::optional<VersionEdit>> {
        cleanup_finished_outputs();
        return Result<std::optional<VersionEdit>>::fail(std::move(status));
        };

    auto add_input_table = [&](const TableMeta& table_meta,
        std::uint32_t level) -> Status {
            if (table_meta.table_id > std::numeric_limits<std::uint32_t>::max()) {
                return Status{
                    StatusCode::InvalidState,
                    "SSTableManager currently supports only 32-bit input table IDs"
                };
            }

            Result<std::shared_ptr<SSTable>> sstable_result =
                sstable_manager.get(
                    static_cast<std::uint32_t>(table_meta.table_id),
                    arena
                );

            if (!sstable_result.is_ok()) {
                return std::move(sstable_result.status);
            }

            if (!sstable_result.value) {
                return Status{
                    StatusCode::NotFound,
                    "SSTable not found during compaction"
                };
            }

            pinned_tables.push_back(std::move(sstable_result.value));
            SSTable& sstable = *pinned_tables.back();

            Result<std::unique_ptr<ReadableFile>> file_result =
                open_readable_file(sstable.get_final_path());
            if (!file_result.is_ok()) {
                return std::move(file_result.status);
            }

            iterators.emplace_back(
                sstable,
                std::move(file_result.value),
                arena
            );

            // SSTableIterator starts invalid. Initialize it explicitly rather than
            // relying on undocumented MergeIterator::build behavior.
            Status seek_status = iterators.back().seek_to_first();
            if (!seek_status.is_ok()) {
                iterators.pop_back();
                pinned_tables.pop_back();
                return seek_status;
            }

            edit.payload.deleted_tables.emplace_back(table_meta.table_id, level);
            return Status::ok();
        };

    for (const auto& table : plan.source_tables) {
        Status status = add_input_table(table, plan.source_level);
        if (!status.is_ok()) {
            return fail(std::move(status));
        }
    }

    for (const auto& table : plan.overlapping_tables) {
        Status status = add_input_table(table, plan.target_level);
        if (!status.is_ok()) {
            return fail(std::move(status));
        }
    }

    if (iterators.empty()) {
        return Result<std::optional<VersionEdit>>::ok(std::nullopt);
    }

    MergeIterator merge_iterator;
    merge_iterator.build(iterators);

    std::unique_ptr<SSTableStreamingBuilder> builder;
    std::uint64_t next_output_table_id = manifest.next_table_id();

    auto start_new_output = [&]() -> Status {
        if (builder) {
            return Status{
                StatusCode::InvalidState,
                "Tried to start an output while another builder is active"
            };
        }

        if (next_output_table_id > std::numeric_limits<std::uint32_t>::max()) {
            return Status{
                StatusCode::InvalidState,
                "SSTableManager currently supports only 32-bit table IDs"
            };
        }

        builder = sstable_manager.create_streaming_builder(
            static_cast<std::uint32_t>(next_output_table_id)
        );
        if (!builder) {
            return Status{
                StatusCode::InvalidState,
                "SSTableManager returned a null streaming builder"
            };
        }

        ++next_output_table_id;
        return Status::ok();
        };

    auto finish_current_output = [&]() -> Status {
        if (!builder) {
            return Status::ok();
        }

        if (builder->empty()) {
            builder.reset();
            return Status::ok();
        }

        Result<std::optional<TableMeta>> meta_result =
            builder->finish(plan.target_level, arena);
        if (!meta_result.is_ok()) {
            return std::move(meta_result.status);
        }

        if (!meta_result.value.has_value()) {
            return Status{
                StatusCode::InvalidState,
                "Non-empty compaction output produced no TableMeta"
            };
        }

        edit.payload.new_tables.push_back(
            std::move(meta_result.value.value())
        );
        builder.reset();
        return Status::ok();
        };

    const bool bottommost = is_bottommost_destination(
        level_manager,
        plan.target_level
    );

    while (merge_iterator.valid()) {
        const InternalRecord& newest = merge_iterator.record();
        const ArenaEntry current_key = newest.key_entry;

        if (compaction_keep_newest_record(newest.type, bottommost)) {
            if (!builder) {
                Status start_status = start_new_output();
                if (!start_status.is_ok()) {
                    return fail(std::move(start_status));
                }
            }

            Status add_status = builder->add(newest);
            if (!add_status.is_ok()) {
                return fail(std::move(add_status));
            }
        }

        // No-snapshot policy: discard every older version of this user key.
        merge_iterator.next();
        while (merge_iterator.valid() &&
            merge_iterator.record().key_entry == current_key) {
            merge_iterator.next();
        }

        if (builder &&
            !builder->empty() &&
            builder->approximate_disk_space() >= plan.max_output_file_size) {
            Status finish_status = finish_current_output();
            if (!finish_status.is_ok()) {
                return fail(std::move(finish_status));
            }
        }
    }

    // If MergeIterator stopped because an underlying iterator failed, do not
    // commit a partial compaction.
    for (const auto& iterator : iterators) {
        Status iterator_status = iterator.status();
        if (!iterator_status.is_ok()) {
            return fail(std::move(iterator_status));
        }
    }

    Status finish_status = finish_current_output();
    if (!finish_status.is_ok()) {
        return fail(std::move(finish_status));
    }

    if (next_output_table_id != manifest.next_table_id()) {
        edit.payload.next_table_id = next_output_table_id;
    }

    if (edit.payload.deleted_tables.empty() &&
        edit.payload.new_tables.empty()) {
        return Result<std::optional<VersionEdit>>::ok(std::nullopt);
    }

    // Pick/run/Manifest::commit must be serialized. The job reserves IDs from
    // manifest.next_table_id() and records the next value in this edit.
    return Result<std::optional<VersionEdit>>::ok(std::move(edit));
}
