#include "sstable_writer.h"
#include "sstable_loader.h"
#include "sstable_builder_options.h"
#include <vector>
#include "sstable_builder.h"
#include "file.h"
#include "status.h"
#include "sstable_iterator.h"
#include "table_meta.h"
#include "arena.h"
#include <memory>

class SSTableManager
{
private:
	std::filesystem::path db_dir;

	std::unordered_map<std::uint32_t, std::shared_ptr<SSTable>> pool;
	
	static std::filesystem::path make_table_path(
		std::uint32_t table_id,
		const std::filesystem::path& dir
	);

	static std::filesystem::path make_tmp_table_path(
		std::uint32_t table_id,
		const std::filesystem::path& dir
	);

public:
	explicit SSTableManager(std::filesystem::path& db_dir);
	explicit SSTableManager(std::filesystem::path&& db_dir);

	Result<std::optional<SSTable>> build(
		std::uint32_t table_id,
		 MemTable& mem_table,
		Arena& arena
	);

	Result<std::optional<SSTable>> build(
		std::uint32_t table_id,
		 SSTableIterator& iterator,
		Arena& arena
	);

	Result<std::shared_ptr<SSTable>> open(
		std::uint32_t table_id,
		Arena& arena
	);

	Result<std::shared_ptr<SSTable>> open(
		 TableMeta& meta,
		Arena& arena
	);

	Result<std::shared_ptr<SSTable>> open_impl(
		std::uint32_t table_id,
		 std::filesystem::path& path,
		Arena& arena
	);

	Result<std::shared_ptr<SSTable>> open_impl(
		std::uint32_t table_id,
		std::filesystem::path&& path,
		Arena& arena
	);

	Status write(SSTable& sstable);

	Result<std::shared_ptr<SSTable>> get(std::uint32_t table_id, Arena& arena);

	std::unique_ptr<SSTableStreamingBuilder> create_streaming_builder(
		std::uint32_t table_id
	);
};