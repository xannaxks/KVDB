// Engine orchestration:
// Recovery publishes no state until options, directories, manifest replay, WAL
// replay, and append preparation all succeed. Writes hold the engine mutex while
// assigning a sequence, appending/syncing the WAL, applying to the MemTable, and
// checking flush/compaction thresholds. Flush and compaction publish files by a
// manifest commit before retiring their old in-memory or on-disk inputs.
#include "engine.h"

#include "compaction_job.h"
#include "compaction_options.h"

#include <algorithm>
#include <format>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
    [[nodiscard]] Result<std::optional<std::string>> api_value(
        const InternalRecord& record
    )
    {
        if (record.type == Type::Tombstone) {
            return Result<std::optional<std::string>>::ok(std::nullopt);
        }

        if (record.type != Type::Put ||
            (record.value_entry.size > 0 &&
                record.value_entry.data == nullptr)) {
            return Result<std::optional<std::string>>::fail(Status{
                StatusCode::Corruption,
                "database record has an invalid type or value"
            });
        }

        if (record.value_entry.size == 0) {
            return Result<std::optional<std::string>>::ok(
                std::string{}
            );
        }

        return Result<std::optional<std::string>>::ok(
            std::string(
                static_cast<const char*>(record.value_entry.data),
                record.value_entry.size
            )
        );
    }

    [[nodiscard]] CompactionOptions scheduler_options(
        const DBOptions& options
    )
    {
        CompactionOptions result;
        result.max_levels = options.compaction.max_levels;
        result.l0_file_count_trigger = static_cast<std::uint32_t>(
            options.compaction.l0_file_count_trigger
        );
        result.max_bytes_per_level =
            options.compaction.max_bytes_per_level;
        result.target_file_size_per_level =
            options.compaction.target_file_size_per_level;
        return result;
    }

    [[nodiscard]] Status filesystem_error(
        std::string operation,
        const std::filesystem::path& path,
        const std::error_code& error
    )
    {
        return Status{
            StatusCode::IOError,
            std::move(operation) + " " + path.string() + ": " +
                error.message()
        };
    }

    [[nodiscard]] Status remove_if_present(
        const std::filesystem::path& path
    )
    {
        std::error_code error;
        (void)std::filesystem::remove(path, error);
        if (error) {
            return filesystem_error("could not remove", path, error);
        }
        return Status::ok();
    }
}

Engine::Engine(DBOptions options)
    : options_(std::move(options))
{
}

Engine::~Engine()
{
    (void)close();
}

std::filesystem::path Engine::wal_path(std::uint32_t wal_id) const
{
    return options_.db_path /
        std::format("wal-{:09}.log", wal_id);
}

Status Engine::prepare_dirs()
{
    namespace fs = std::filesystem;

    std::error_code error;
    const bool existed = fs::exists(options_.db_path, error);
    if (error) {
        return filesystem_error(
            "could not inspect database directory",
            options_.db_path,
            error
        );
    }

    if (existed) {
        if (!fs::is_directory(options_.db_path, error)) {
            if (error) {
                return filesystem_error(
                    "could not inspect database path",
                    options_.db_path,
                    error
                );
            }
            return Status{
                StatusCode::InvalidArgument,
                "database path exists but is not a directory"
            };
        }

        if (options_.error_if_exists) {
            return Status{
                StatusCode::AlreadyExists,
                "database directory already exists"
            };
        }
    }
    else {
        if (!options_.create_if_missing) {
            return Status{
                StatusCode::NotFound,
                "database directory does not exist"
            };
        }

        fs::create_directories(options_.db_path, error);
        if (error) {
            return filesystem_error(
                "could not create database directory",
                options_.db_path,
                error
            );
        }
    }

    sstable_dir_ = options_.db_path / "sstables";
    fs::create_directories(sstable_dir_, error);
    if (error) {
        return filesystem_error(
            "could not create SSTable directory",
            sstable_dir_,
            error
        );
    }

    manifest_path_ = options_.db_path / "MANIFEST";
    return Status::ok();
}

Status Engine::open_manifest()
{
    std::error_code error;
    const bool exists = std::filesystem::exists(manifest_path_, error);
    if (error) {
        return filesystem_error(
            "could not inspect manifest",
            manifest_path_,
            error
        );
    }

    std::uintmax_t size = 0;
    if (exists) {
        size = std::filesystem::file_size(manifest_path_, error);
        if (error) {
            return filesystem_error(
                "could not inspect manifest size",
                manifest_path_,
                error
            );
        }
    }

    if (exists && size > 0) {
        Result<Manifest> loaded = Manifest::load(
            *level_manager_,
            manifest_path_,
            *arena_
        );
        if (!loaded.is_ok()) {
            return std::move(loaded.status);
        }

        manifest_ = std::make_unique<Manifest>(
            std::move(loaded.value)
        );

        Status status = manifest_->prepare_for_append();
        if (!status.is_ok()) {
            return status;
        }

        Result<std::unique_ptr<WritableFile>> writer =
            open_writable_file(manifest_path_);
        if (!writer.is_ok()) {
            return std::move(writer.status);
        }

        status = manifest_->attach_writer(std::move(writer.value));
        if (!status.is_ok()) {
            return status;
        }
        return Status::ok();
    }

    manifest_ = std::make_unique<Manifest>(manifest_path_);
    return manifest_->open_or_create();
}

Status Engine::open_wal()
{
    const std::uint64_t manifest_wal_id = manifest_->current_wal_id();
    if (manifest_wal_id == 0 ||
        manifest_wal_id > std::numeric_limits<std::uint32_t>::max()) {
        return Status{
            StatusCode::Corruption,
            "manifest WAL ID is outside the supported range"
        };
    }

    current_wal_id_ = static_cast<std::uint32_t>(manifest_wal_id);
    next_sequence_ = manifest_->next_sequence_number();

    const std::filesystem::path current_path =
        wal_path(current_wal_id_);

    std::error_code error;
    const bool exists = std::filesystem::exists(current_path, error);
    if (error) {
        return filesystem_error(
            "could not inspect WAL",
            current_path,
            error
        );
    }

    if (!exists) {
        Status remove_status = remove_if_present(current_path);
        if (!remove_status.is_ok()) {
            return remove_status;
        }

        wal_ = std::make_unique<WAL>();
        return wal_->create(
            current_path,
            current_wal_id_,
            next_sequence_
        );
    }

    Result<WALLoader::LoadResult> recovered = WAL::recover(
        current_path,
        current_wal_id_,
        *arena_
    );
    if (!recovered.is_ok()) {
        return std::move(recovered.status);
    }
    if (!recovered.value.ok || recovered.value.had_corruption) {
        return Status{
            StatusCode::Corruption,
            recovered.value.error.empty()
                ? "WAL recovery detected corruption"
                : recovered.value.error
        };
    }

    for (const InternalRecord& record : recovered.value.records) {
        Status status = mem_table_->apply(record);
        if (!status.is_ok()) {
            return status;
        }

        if (record.seq_num == std::numeric_limits<std::uint64_t>::max()) {
            return Status{
                StatusCode::InvariantViolation,
                "WAL sequence number space is exhausted"
            };
        }
        next_sequence_ = std::max(
            next_sequence_,
            record.seq_num + 1
        );
    }

    if (current_wal_id_ == std::numeric_limits<std::uint32_t>::max()) {
        return Status{
            StatusCode::InvariantViolation,
            "WAL ID space is exhausted"
        };
    }

    const std::uint32_t replacement_id = current_wal_id_ + 1;
    const std::filesystem::path replacement_path =
        wal_path(replacement_id);

    Status status = remove_if_present(replacement_path);
    if (!status.is_ok()) {
        return status;
    }

    auto replacement = std::make_unique<WAL>();
    status = replacement->create(
        replacement_path,
        replacement_id,
        recovered.value.records.empty()
            ? next_sequence_
            : recovered.value.records.front().seq_num
    );
    if (!status.is_ok()) {
        return status;
    }

    for (const InternalRecord& record : recovered.value.records) {
        status = replacement->write(record);
        if (!status.is_ok()) {
            (void)replacement->close();
            (void)remove_if_present(replacement_path);
            return status;
        }
    }

    status = replacement->sync();
    if (!status.is_ok()) {
        (void)replacement->close();
        return status;
    }

    VersionEdit edit;
    edit.payload.current_wal_id = replacement_id;
    edit.payload.next_sequence_number = next_sequence_;
    status = manifest_->commit(*level_manager_, edit);
    if (!status.is_ok()) {
        (void)replacement->close();
        return status;
    }

    wal_ = std::move(replacement);
    current_wal_id_ = replacement_id;

    status = remove_if_present(current_path);
    if (!status.is_ok()) {
        return status;
    }
    return Status::ok();
}

Status Engine::open()
{
    std::lock_guard lock(mutex_);

    if (opened_) {
        return Status::ok();
    }
    if (closed_) {
        return Status{
            StatusCode::UseAfterClose,
            "database is closed"
        };
    }

    Status status = options_.validate();
    if (!status.is_ok()) {
        return status;
    }

    status = prepare_dirs();
    if (!status.is_ok()) {
        return status;
    }

    try {
        arena_ = std::make_unique<Arena>(
            options_.arena.page_size,
            options_.arena.large_threshold
        );
        level_manager_ = std::make_unique<LevelManager>(
            options_.compaction.max_levels
        );
        mem_table_ = std::make_unique<MemTable>();
        compaction_scheduler_ =
            std::make_unique<CompactionScheduler>();
        sstable_manager_ =
            std::make_unique<SSTableManager>(sstable_dir_);
    }
    catch (const std::bad_alloc&) {
        return Status{
            StatusCode::OutOfMemory,
            "could not allocate database runtime state"
        };
    }

    status = open_manifest();
    if (!status.is_ok()) {
        return status;
    }

    status = open_wal();
    if (!status.is_ok()) {
        return status;
    }

    opened_ = true;
    return Status::ok();
}

Status Engine::put_impl(
    std::string_view key,
    std::string_view value,
    Type type
)
{
    Status state = ensure_open();
    if (!state.is_ok()) {
        return state;
    }
    if (type != Type::Put && type != Type::Tombstone) {
        return Status{
            StatusCode::InvalidArgument,
            "invalid record type"
        };
    }
    if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
        return Status{
            StatusCode::InvariantViolation,
            "sequence number space is exhausted"
        };
    }

    const Arena::Checkpoint checkpoint = arena_->checkpoint();

    Result<ArenaEntry> key_entry = ArenaEntry::make_entry(
        *arena_,
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(key.data()),
            key.size()
        )
    );
    if (!key_entry.is_ok()) {
        return std::move(key_entry.status);
    }

    Result<ArenaEntry> value_entry =
        type == Type::Tombstone
        ? Result<ArenaEntry>::ok(ArenaEntry{})
        : ArenaEntry::make_entry(
            *arena_,
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(value.data()),
                value.size()
            )
        );
    if (!value_entry.is_ok()) {
        arena_->rollback(checkpoint);
        return std::move(value_entry.status);
    }

    InternalRecord record(
        key_entry.value,
        value_entry.value,
        type,
        next_sequence_
    );

    Status status = wal_->write(record);
    if (!status.is_ok()) {
        arena_->rollback(checkpoint);
        return status;
    }

    if (options_.wal.sync_on_write) {
        status = wal_->sync();
        if (!status.is_ok()) {
            return status;
        }
    }

    status = mem_table_->apply(record);
    if (!status.is_ok()) {
        return status;
    }

    ++next_sequence_;
    return maybe_flush_unlocked();
}

Status Engine::put(std::string& key, std::string& value)
{
    std::lock_guard lock(mutex_);
    return put_impl(key, value, Type::Put);
}

Status Engine::remove(std::string& key)
{
    std::lock_guard lock(mutex_);
    return put_impl(key, {}, Type::Tombstone);
}

Result<std::optional<std::string>> Engine::get(std::string_view key)
{
    std::lock_guard lock(mutex_);

    Status state = ensure_open();
    if (!state.is_ok()) {
        return Result<std::optional<std::string>>::fail(
            std::move(state)
        );
    }

    if (key.size() > std::numeric_limits<std::uint32_t>::max()) {
        return Result<std::optional<std::string>>::fail(Status{
            StatusCode::InvalidArgument,
            "key is too large"
        });
    }

    ArenaEntry search_key(
        key.data(),
        key.size()
    );

    Result<std::optional<InternalRecord>> memory_result =
        mem_table_->get(search_key);
    if (!memory_result.is_ok()) {
        return Result<std::optional<std::string>>::fail(
            std::move(memory_result.status)
        );
    }
    if (memory_result.value.has_value()) {
        return api_value(*memory_result.value);
    }

    Arena read_arena(
        options_.arena.page_size,
        options_.arena.large_threshold
    );

    for (std::uint32_t level = 0;
        level < level_manager_->level_count();
        ++level) {
        Result<std::vector<TableMeta>> candidates =
            level_manager_->find_candidate_tables_in_level(
                level,
                search_key
            );
        if (!candidates.is_ok()) {
            return Result<std::optional<std::string>>::fail(
                std::move(candidates.status)
            );
        }

        for (const TableMeta& meta : candidates.value) {
            Result<std::shared_ptr<SSTable>> table =
                sstable_manager_->open(meta, *arena_);
            if (!table.is_ok()) {
                return Result<std::optional<std::string>>::fail(
                    std::move(table.status)
                );
            }

            Result<std::optional<InternalRecord>> record =
                table.value->get(search_key, read_arena);
            if (!record.is_ok()) {
                if (record.status.code == StatusCode::NotFound) {
                    continue;
                }
                return Result<std::optional<std::string>>::fail(
                    std::move(record.status)
                );
            }
            if (record.value.has_value()) {
                return api_value(*record.value);
            }
        }
    }

    return Result<std::optional<std::string>>::ok(std::nullopt);
}

Status Engine::flush_oldest_immutable()
{
    Result<MemTable::ImmutableSnapshot> snapshot =
        mem_table_->oldest_immutable();
    if (!snapshot.is_ok()) {
        return snapshot.status.code == StatusCode::NotFound
            ? Status::ok()
            : std::move(snapshot.status);
    }

    const std::uint64_t next_table_id = manifest_->next_table_id();
    if (next_table_id == 0 ||
        next_table_id > std::numeric_limits<std::uint32_t>::max()) {
        return Status{
            StatusCode::InvariantViolation,
            "table ID space is exhausted"
        };
    }
    const auto table_id = static_cast<std::uint32_t>(next_table_id);

    Result<std::optional<SSTable>> built =
        sstable_manager_->build(table_id, *mem_table_);
    if (!built.is_ok()) {
        return std::move(built.status);
    }
    if (!built.value.has_value()) {
        return Status{
            StatusCode::InvariantViolation,
            "non-empty immutable MemTable produced no SSTable"
        };
    }

    SSTable& table = *built.value;
    Status status = sstable_manager_->write(table);
    if (!status.is_ok()) {
        return status;
    }

    Result<TableMeta> meta = make_table_meta(
        table,
        0,
        *arena_
    );
    if (!meta.is_ok()) {
        return std::move(meta.status);
    }

    VersionEdit edit;
    edit.payload.new_tables.push_back(std::move(meta.value));
    edit.payload.next_table_id = next_table_id + 1;
    edit.payload.next_sequence_number = next_sequence_;

    std::unique_ptr<WAL> replacement;
    std::filesystem::path old_wal_path;
    std::filesystem::path replacement_path;
    std::uint32_t replacement_id = 0;

    const bool rotates_wal = mem_table_->immutable_count() == 1;
    if (rotates_wal) {
        if (current_wal_id_ ==
            std::numeric_limits<std::uint32_t>::max()) {
            return Status{
                StatusCode::InvariantViolation,
                "WAL ID space is exhausted"
            };
        }

        replacement_id = current_wal_id_ + 1;
        replacement_path = wal_path(replacement_id);
        old_wal_path = wal_path(current_wal_id_);

        status = remove_if_present(replacement_path);
        if (!status.is_ok()) {
            return status;
        }

        replacement = std::make_unique<WAL>();
        status = replacement->create(
            replacement_path,
            replacement_id,
            next_sequence_
        );
        if (!status.is_ok()) {
            return status;
        }
        edit.payload.current_wal_id = replacement_id;
    }

    status = manifest_->commit(*level_manager_, edit);
    if (!status.is_ok()) {
        if (replacement) {
            (void)replacement->close();
            (void)remove_if_present(replacement_path);
        }
        return status;
    }

    if (!mem_table_->retire_oldest_immutable(
        snapshot.value.generation_id)) {
        return Status{
            StatusCode::InvariantViolation,
            "flushed immutable MemTable generation changed unexpectedly"
        };
    }

    if (replacement) {
        status = wal_->close();
        if (!status.is_ok()) {
            return status;
        }
        wal_ = std::move(replacement);
        current_wal_id_ = replacement_id;

        status = remove_if_present(old_wal_path);
        if (!status.is_ok()) {
            return status;
        }
    }

    return Status::ok();
}

Status Engine::flush_unlocked()
{
    Status state = ensure_open();
    if (!state.is_ok()) {
        return state;
    }

    Status status = mem_table_->freeze_mutable();
    if (!status.is_ok()) {
        return status;
    }

    while (mem_table_->has_immutable()) {
        status = flush_oldest_immutable();
        if (!status.is_ok()) {
            return status;
        }
    }

    return maybe_compact_unlocked();
}

Status Engine::flush()
{
    std::lock_guard lock(mutex_);
    return flush_unlocked();
}

Status Engine::maybe_flush_unlocked()
{
    if (mem_table_->mutable_memory_usage() >=
        options_.memtable.size_limit ||
        wal_->writer().offset() >= options_.wal.file_size_limit) {
        return flush_unlocked();
    }
    return Status::ok();
}

Status Engine::run_compaction(const CompactionPlan& plan)
{
    CompactionJob job;
    Result<std::optional<VersionEdit>> result = job.run(
        plan,
        *level_manager_,
        *manifest_,
        *sstable_manager_,
        *arena_
    );
    if (!result.is_ok()) {
        return std::move(result.status);
    }
    if (!result.value.has_value()) {
        return Status::ok();
    }

    std::vector<std::filesystem::path> obsolete_paths;
    obsolete_paths.reserve(
        plan.source_tables.size() +
        plan.overlapping_tables.size()
    );
    for (const TableMeta& table : plan.source_tables) {
        obsolete_paths.push_back(table.path);
    }
    for (const TableMeta& table : plan.overlapping_tables) {
        obsolete_paths.push_back(table.path);
    }

    Status status = manifest_->commit(
        *level_manager_,
        *result.value
    );
    if (!status.is_ok()) {
        return status;
    }

    for (const auto& path : obsolete_paths) {
        status = remove_if_present(path);
        if (!status.is_ok()) {
            return status;
        }
    }
    return Status::ok();
}

Status Engine::maybe_compact_unlocked()
{
    if (!options_.compaction.enable_background_compaction) {
        return Status::ok();
    }

    const CompactionOptions options = scheduler_options(options_);
    while (compaction_scheduler_->should_compact(
        *level_manager_,
        options
    )) {
        Result<std::optional<CompactionPlan>> plan =
            compaction_scheduler_->pick_compaction(
                *level_manager_,
                options
            );
        if (!plan.is_ok()) {
            return std::move(plan.status);
        }
        if (!plan.value.has_value()) {
            break;
        }

        Status status = run_compaction(*plan.value);
        if (!status.is_ok()) {
            return status;
        }
    }
    return Status::ok();
}

Status Engine::compact_range(
    std::string_view begin,
    std::string_view end
)
{
    std::lock_guard lock(mutex_);

    if (begin.size() > std::numeric_limits<std::uint32_t>::max() ||
        end.size() > std::numeric_limits<std::uint32_t>::max()) {
        return Status{
            StatusCode::InvalidArgument,
            "compaction range key is too large"
        };
    }

    Status status = flush_unlocked();
    if (!status.is_ok()) {
        return status;
    }

    ArenaEntry smallest(
        begin.data(),
        begin.size()
    );
    ArenaEntry largest(
        end.data(),
        end.size()
    );
    if (largest < smallest) {
        return Status{
            StatusCode::InvalidArgument,
            "compaction range end precedes its begin"
        };
    }

    const CompactionOptions options = scheduler_options(options_);

    for (std::uint32_t source_level = 0;
        source_level + 1 < level_manager_->level_count();
        ++source_level) {
        CompactionPlan plan;
        plan.reason = CompactionReason::Manual;
        plan.source_level = source_level;
        plan.target_level = source_level + 1;
        plan.smallest_key = smallest;
        plan.largest_key = largest;
        plan.max_output_file_size =
            options.target_file_size_per_level[plan.target_level];

        for (;;) {
            const std::size_t old_source_count =
                plan.source_tables.size();
            const std::size_t old_target_count =
                plan.overlapping_tables.size();
            const ArenaEntry old_smallest = plan.smallest_key;
            const ArenaEntry old_largest = plan.largest_key;

            plan.source_tables =
                level_manager_->find_overlapping_tables(
                    plan.source_level,
                    plan.smallest_key,
                    plan.largest_key
                );
            if (plan.source_tables.empty()) {
                break;
            }

            for (const TableMeta& table : plan.source_tables) {
                if (table.smallest_key < plan.smallest_key) {
                    plan.smallest_key = table.smallest_key;
                }
                if (plan.largest_key < table.largest_key) {
                    plan.largest_key = table.largest_key;
                }
            }

            plan.overlapping_tables =
                level_manager_->find_overlapping_tables(
                    plan.target_level,
                    plan.smallest_key,
                    plan.largest_key
                );
            for (const TableMeta& table : plan.overlapping_tables) {
                if (table.smallest_key < plan.smallest_key) {
                    plan.smallest_key = table.smallest_key;
                }
                if (plan.largest_key < table.largest_key) {
                    plan.largest_key = table.largest_key;
                }
            }

            if (old_source_count == plan.source_tables.size() &&
                old_target_count == plan.overlapping_tables.size() &&
                old_smallest == plan.smallest_key &&
                old_largest == plan.largest_key) {
                break;
            }
        }

        if (plan.source_tables.empty()) {
            continue;
        }
        if (!plan.validate()) {
            return Status{
                StatusCode::InvariantViolation,
                "manual compaction produced an invalid plan"
            };
        }

        status = run_compaction(plan);
        if (!status.is_ok()) {
            return status;
        }
    }

    return Status::ok();
}

Status Engine::close()
{
    std::lock_guard lock(mutex_);

    if (closed_) {
        return Status::ok();
    }
    if (!opened_) {
        closed_ = true;
        return Status::ok();
    }

    Status status = wal_ ? wal_->sync() : Status::ok();
    if (!status.is_ok()) {
        return status;
    }

    status = manifest_ ? manifest_->sync() : Status::ok();
    if (!status.is_ok()) {
        return status;
    }

    status = wal_ ? wal_->close() : Status::ok();
    if (!status.is_ok()) {
        return status;
    }

    opened_ = false;
    closed_ = true;
    return Status::ok();
}

Status Engine::ensure_open() const
{
    if (closed_) {
        return Status{
            StatusCode::UseAfterClose,
            "database is closed"
        };
    }
    if (!opened_) {
        return Status{
            StatusCode::FailedPrecondition,
            "database is not open"
        };
    }
    return Status::ok();
}
