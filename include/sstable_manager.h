
class SSTableManager
{
public:
	SSTableManager() = default;

private:
	SSTableWriter sstable_writer;
	SSTableLoader sstable_loader;

	std::vector<SSTable> immutable_pool;
	std::vector<SSTable> pool;

	static std::filesystem::path make_table_path(const std::filesystem::path& dir, std::uint32_t table_id);
	static std::filesystem::path make_tmp_table_path(const std::filesystem::path& dir, std::uint32_t table_id);

public:

	Status write_latest(bool erase = false);
	std::vector<Status> write_all();
	void add_to_pool(SSTable&& sstable);
	Status add_to_pool(MemTable& memtable);
	//void add_to_pool(MemTable& mem_table);
	std::vector<Status> load(Arena& arena, const std::filesystem::path& root_path);
	Result<std::optional<InternalRecord>> get_latest(std::string_view key);
	//void get_latest();
};