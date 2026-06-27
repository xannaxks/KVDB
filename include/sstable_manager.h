#include "sstable_writer.h"
#include "sstable_loader.h"
#include "sstable_builder_options.h"
#include <vector>
#include "file.h"
#include "status.h"
#include "sstable_iterator.h"
#include "table_meta.h"
#include "arena.h"
#include <memory>

class SSTableManager
{
public:
	explicit SSTableManager(std::filesystem::path db_dir);

	Result<std::optional<TableMeta>> create_from_memtable(
		std::uint32_t table_id,
		const MemTable& mem_table,
		Arena& arena
	);

	Result<TableMeta> create_from_iterator(
		std::uint32_t table_id,
		SSTableIterator& iterator, 
		Arena& arena
	);

	Result<std::shared_ptr<SSTable>> open(
		const TableMeta& meta,
		Arena& arena
	);

	Status remove(std::uint32_t table_id);

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
};