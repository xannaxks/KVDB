#include "engine.h"
#include <cassert>

Engine::Engine(DBOptions options)
	: options_(std::move(options)),
	manifest_(std::make_unique<Manifest>(options_)),
	wal_(std::make_unique<WAL>(options_)),
	mem_table_(std::make_unique<MemTable>(options_)),
	compaction_scheduler_(std::make_unique<CompactionScheduler>(options_)),
	sstable_manager_(std::make_unique<SSTableManager>(options_)), 
	level_manager_(std::make_unique<LevelManager>(options_))
{
}

Status Engine::put(std::string& key, std::string& value)
{
    if (closed_) {
        return Status{StatusCode::UseAfterClose, "database is closed"};
    }

    const std::uint64_t seq = last_seq_num_ + 1;

	InternalRecord record{};
	record.key_entry = ArenaEntry(reinterpret_cast<void*>(key.data()), key.size());
	record.value_entry = ArenaEntry(reinterpret_cast<void*>(value.data()), value.size());
	record.seq_num = seq;
	record.type = ::Type::Put;

	return this->put_impl(key, value, record);
}

Status Engine::remove(std::string& key)
{
	if (this->closed_)
		return Status{
			StatusCode::UseAfterClose,
			"Attempt to use closed engine was denied"
		};

	InternalRecord record{};
	record.key_entry = ArenaEntry(reinterpret_cast<void*>(key.data()), key.size());
	record.value_entry = ArenaEntry(nullptr, 0);
	record.seq_num = this->last_seq_ + 1;
	record.type = ::Type::Tombstone;

	return this->put_impl(key, "", record);
}

Status Engine::put_impl(const std::string& key, const std::string& value, const InternalRecord& record)
{
	Status s = wal_->append(record);
	if (!s.is_ok()) {
		return s;
	}

	s = mem_table_->put(key, value, record.seq_num);
	if (!s.is_ok()) {
		return s;
	}

	last_seq_ = record.seq_num;

	if (mem_table_->should_flush()) {
		return flush();
	}

	return Status::ok();
}

Result<std::optional<std::string>>
construct_response_for_api(const InternalRecord& record)
{
	if (record.type == Type::Tombstone) {
		return Result<std::optional<std::string>>::ok(std::nullopt);
	}

	const auto& value = record.value_entry;

	std::string value_str(
		reinterpret_cast<const char*>(value.data),
		value.size
	);

	return Result<std::optional<std::string>>::ok(std::move(value_str));
}

Result<std::optional<std::string>> Engine::get(std::string_view key)
{
	if (closed_) {
		return Result<std::optional<std::string>>::fail(
			Status{
				StatusCode::UseAfterClose,
				"Attempt to use closed engine was denied"
			}
		);
	}

	auto result = mem_table_->get(key);

	if (result.is_ok()) {
		return construct_response_for_api(*result.value);
	}

	if (result.status.code != StatusCode::NotFound) {
		return Result<std::optional<std::string>>::fail(result.status);
	}

	// Future:
	// search immutable memtables newest -> oldest

	result = sstable_manager_->get_latest(key);

	if (result.is_ok()) {
		return construct_response_for_api(*result.value);
	}

	if (result.status.code == StatusCode::NotFound) {
		return Result<std::optional<std::string>>::ok(std::nullopt);
	}

	return Result<std::optional<std::string>>::fail(result.status);
}

Status Engine::flush()
{
	if (closed_) {
		return Status{ StatusCode::UseAfterClose, "database is closed" };
	}

	if (mem_table_->empty()) {
		return Status::ok();
	}

	const std::uint32_t table_id = allocate_next_table_id();

	auto build_result = sstable_manager_->build(
		table_id,
		*mem_table_
	);

	if (!build_result.is_ok()) {
		return build_result.status;
	}

	Result<TableMeta> meta_result = make_table_meta(*build_result.value, 0, arena);
	if (!meta_result.is_ok()) {
		return meta_result.status;
	}
	TableMeta& meta = meta_result.value;

	VersionEdit edit;
	edit.add_table(meta);

	Status s = manifest_->commit(edit); // commit syncs
	if (!s.is_ok()) {
		return s;
	}

	//s = manifest_->sync();
	//if (!s.is_ok()) {
	//	return s;
	//}

	s = level_manager_->add_table(std::move(meta));
	if (!s.is_ok()) {
		return s;
	}

	mem_table_ = std::make_unique<MemTable>(options_.mem_table_options);
	// WAL rotation/recycling should happen after the flush is durable.
	return wal_->reset();
}

Status Engine::compact_range(
	std::string_view begin,
	std::string_view end)
{
	if (closed_) {
		return Status{ StatusCode::UseAfterClose, "database is closed" };
	}

	// Optional but usually good:
	// flush current memtable first, so newest writes are included.
	Status s = flush();
	if (!s.is_ok()) {
		return s;
	}

	//CompactionInput input = level_manager_->pick_range_compaction(begin, end);

	//if (input.empty()) {
	//	return Status::ok();
	//}

	//CompactionJob job{
	//	.input = std::move(input),
	//	.begin = std::string(begin),
	//	.end = std::string(end),
	//};

	//return compaction_scheduler_->run_manual(job);  =====> exact implementation coming soon
}

Status Engine::prepare_dirs()
{
	namespace fs = std::filesystem;

	const fs::path& db_dir = options_.db_path;
	const fs::path& sst_dir = options_.sstable_dir;

	std::error_code ec;

	auto ensure_dir = [&](const fs::path& dir) -> Status
	{
		if (dir.empty())
			return Status{ StatusCode::InvalidArgument, "directory path is empty" };
		
		if (fs::exists(dir, ec))
		{
			if (ec)
				return Status{
					StatusCode::IOError,
					"failed to check directory existence: " + dir.string() +
					", error: " + ec.message()
				};

			if (!fs::is_directory(dir, ec))
				return Status{
					StatusCode::InvalidArgument,
					"path exists, but is not a directory: " + dir.string()
				};

			if (ec)
				return Status{
					StatusCode::IOError,
					"failed to check type whether its directory or file: " + dir.string() +
					", error: " + ec.message()
				};

			return Status::ok();
		}

		fs::create_directories(dir, ec);
		if (ec)
			return Status{
				StatusCode::IOError, 
				"failed to create directory" + dir.string() + ", error: " + ec.message()
			};

		return Status::ok();
	};

	Status is_dir = ensure_dir(db_dir);
	if (!is_dir.is_ok())
		return is_dir;

	Status is_sst_dir = ensure_dir(sst_dir);
	return is_dir;

}


Status Engine::recover_manifest()
{
	Status manifest_recover_status = manifest_->recover(this->options_.manifest_options);
	if(!manifest_recover_status.is_ok())
		return manifest_recover_status;
	
	Status level_manager_recover_status = level_manager_->recover(*manifest_, this->options_.level_manager_options);
	return level_manager_recover_status;
}

Status Engine::recover_wal()
{
	Status wal_recover_status = wal_->recover(this->options_.wal_options);
	if (!wal_recover_status.is_ok())
		return wal_recover_status;

	Status mem_table_recover_status = mem_table_->recover(*wal_, this->options_.mem_table_options);
	return mem_table_recover_status;
}

Status Engine::recover_sstables()
{
	Status sstable_recover_status = sstable_manager_->recover(*level_manager_, this->options_.sstable_manager_options);
	return sstable_recover_status;
}

Status Engine::recover_counters()
{
	
}

Status Engine::recover()
{
	Status dirs_status = this->prepare_dirs();
	if (!dirs_status.is_ok())
		return dirs_status;

	Status manifest_status = this->recover_manifest();
	if (!manifest_status.is_ok())
		return manifest_status;
	
	Status sstable_status = this->recover_sstables();
	if (!sstable_status.is_ok())
		return sstable_status;
	
	Status wal_status = this->recover_wal();
	if (!wal_status.is_ok())
		return wal_status;

	Status counters_status = this->recover_counters();
	return counters_status;
}

Status Engine::open()
{
	if (opened_)
		return Status::ok();

	if (closed_)
		return Status{ StatusCode::UseAfterClose, "database is closed" };
	
	Status recovery_result = this->recover();

	if (!recovery_result.is_ok())
		return recovery_result;

	//Arena arena(this->options_.arena_options.page_size, this->options_.arena_options.large_threshold); // arena options in dboptions

	//Result<Manifest> manifest_result = Manifest::load(this->options_.manifest_options.path, arena);
	//if (!manifest_result.is_ok()) {
	//	return manifest_result.status;
	//}
	//this->manifest_ = std::make_unique<Manifest>(std::move(manifest_result.value));

	//Result<WAL> wal_result = WAL::load(this->options_.wal_options);
	//if (!wal_result.is_ok()) {
	//	return wal_result.status;
	//}
	//this->wal_ = std::make_unique<WAL>(std::move(wal_result.value));

	//this->sstable_manager_ = std::make_unique<SSTableManager>(this->options_.sstable_manager_options);
	//this->level_manager_ = std::make_unique<LevelManager>(this->options_.level_manager_options);
	//this->mem_table_ = std::make_unique<MemTable>(this->options_.mem_table_options);

	return Status::ok();
}

Status Engine::close()
{
	if (closed_)
		return Status::ok();

	if (wal_)
	{
		Status sync_res = wal_->sync();
		if (!sync_res.is_ok())
			return sync_res;
	}

	if (manifest_)
	{
		Status manifest_res = manifest_->sync();
		if (!manifest_res.is_ok())
			return manifest_res;
	}

	closed_ = true;
	return Status::ok();
}

Result<std::uint32_t> Engine::allocate_next_table_id()
{
	Status is_opened = this->ensure_open();
	if (!is_opened.is_ok())
		return Result<std::uint32_t>::fail(std::move(is_opened));

	this->last_table_id_++;
	return Result<std::uint32_t>::ok(this->last_table_id_);
}

Result<std::uint64_t> Engine::allocate_next_sequence()
{
	Status is_opened = this->ensure_open();
	if (!is_opened.is_ok())
		return Result<std::uint64_t>::fail(std::move(is_opened));

	this->last_seq_num_++;
	return Result<std::uint64_t>::ok(this->last_seq_num_);
}

Status Engine::ensure_open() const
{
	if (closed_)
		return Status{
			StatusCode::UseAfterClose,
			"Attempt to use closed engine was denied"
		};

	if(!opened_)
		return Status{
			StatusCode::BadAccess,
			"Attempt to use closed engine was denied"
		};

	return Status::ok();
}
