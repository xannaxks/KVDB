#include "engine.h"

Engine::Engine(DBOptions options)
	: options_(std::move(options)),
	manifest_(std::make_unique<Manifest>(options_)),
	wal_(std::make_unique<WAL>(options_)),
	mutable_mem_table_(std::make_unique<MemTable>()),
	compaction_scheduler_(std::make_unique<CompactionScheduler>()),
	sstable_manager_(std::make_unique<SSTableManager>(options_))
{
}

Status Engine::put(std::string_view key, std::string_view value)
{
	if (this->closed_)
		return Status{
			StatusCode::UseAfterClose,
			"Attempt to use closed engine was denied"
		};

	Status result = this->mutable_mem_table_->put(key, value, this->last_seq_);

	if(result.is_ok())
		this->last_seq_++;
	
	return result;
}

Status Engine::remove(std::string_view key)
{
	if (this->closed_)
		return Status{
			StatusCode::UseAfterClose,
			"Attempt to use closed engine was denied"
		};

	Status result = this->mutable_mem_table_->remove(key, this->last_seq_);

	if(result.is_ok())
		this->last_seq_++;
	
	return result;
}

Result<std::optional<std::string>> construct_response_for_api(std::optional<InternalRecord>& result)
{

	::Type& type = result->type;

	if (type == ::Type::Tombstone)
	{
		return Result<std::optional<std::string>>::fail(
			Status{
				StatusCode::NotFound,
				"Key not found or deleted"
			}
		);
	}

	ArenaEntry& value = result->value_entry;
	std::string value_str;

	for (std::size_t i = 0; i < value.size; i++)
		value_str += (reinterpret_cast<char*>(value.data))[i];

	return Result<std::optional<std::string>>::ok(value_str);
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

	auto result = mutable_mem_table_->get(key);

	if (result.is_ok()) {
		return construct_response_for_api(result.value);
	}

	if (result.status.code != StatusCode::NotFound) {
		return Result<std::optional<std::string>>::fail(result.status);
	}

	// Future:
	// search immutable memtables newest -> oldest

	result = sstable_manager_->get_latest(key);

	if (result.is_ok()) {
		return construct_response_for_api(result.value);
	}

	if (result.status.code == StatusCode::NotFound) {
		return Result<std::optional<std::string>>::ok(std::nullopt);
	}

	return Result<std::optional<std::string>>::fail(result.status);
}

Status Engine::flush()
{
	if (this->closed_)
		return Status
		{
			StatusCode::UseAfterClose,
			"Attempt to use engine after closing was denied"
		};

	Status result = this->sstable_manager_->add_to_pool(*this->mutable_mem_table_);
	if (result.is_ok())
	{
		// freeing memtable occupied space logics
		// wal freeing logics
		return Status::ok();
	}
	return result;
}

Status Engine::compact_range(std::string_view begin,
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