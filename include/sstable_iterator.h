
class SSTableIterator
{
private:
	const SSTable& sstable;
	std::unique_ptr<ReadableFile> file;
	Arena& arena;

	std::uint64_t current_offset = 0;

	std::uint64_t data_block_count = 0;
	std::uint64_t next_block_index = 0;

	std::size_t record_index = 0;
	std::vector<InternalRecord> current_block_records;

	bool valid_ = false;
	Status status_ = Status::ok();

public:
	SSTableIterator() = delete;
	SSTableIterator(const SSTable& sstable, std::unique_ptr<ReadableFile>&& file, Arena& arena);

	Status seek_to_first();
	Status next();

	bool valid() const;
	const InternalRecord& record() const;
	Status status() const;

private:
	Status load_next_block();
};