#include "engine.h"

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

    const std::uint64_t seq = last_seq_ + 1;

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

Status Engine::open()
{
	if (closed_) {
		return Status{ StatusCode::UseAfterClose, "database is closed" };
	}
	
	Arena arena(this->options_.arena_options.page_size, this->options_.arena_options.large_threshold); // arena options in dboptions

	Result<Manifest> manifest_result = Manifest::load(this->options_.manifest_options.path, arena);
	if (!manifest_result.is_ok()) {
		return manifest_result.status;
	}
	this->manifest_ = std::make_unique<Manifest>(std::move(manifest_result.value));

	Result<WAL> wal_result = WAL::load(this->options_.wal_options);
	if (!wal_result.is_ok()) {
		return wal_result.status;
	}
	this->wal_ = std::make_unique<WAL>(std::move(wal_result.value));

	this->sstable_manager_ = std::make_unique<SSTableManager>(this->options_.sstable_manager_options);
	this->level_manager_ = std::make_unique<LevelManager>(this->options_.level_manager_options);
	this->mem_table_ = std::make_unique<MemTable>(this->options_.mem_table_options);

	return Status::ok();
}

Status Engine::close()
{
	Status result = this->wal_->sync();
	if (!result.is_ok()) {
		return result;
	}

	result = this->manifest_->sync();
	if (!result.is_ok()) {
		return result;
	}

	this->closed_ = true;

	return Status::ok();
}

std::uint32_t Engine::allocate_next_table_id()
{
	if (closed_) {
		throw std::runtime_error("Attempt to use closed engine was denied");
	}
	this->last_table_id++;
	return this->last_table_id;
}