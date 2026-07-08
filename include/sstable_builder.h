#include "mem_table.h"
#include "arena.h"
#include "file.h"
#include "table_meta.h"
#include "status.h"
#include "sstable.h"
#include "sstable_entities.h"
#include "sstable_iterator.h"
#include "record.h"

#include <cstdint>
#include <optional>
#include <memory>
#include <vector>
#include <filesystem>

class SSTableBuilder
{
public:

	static Result<std::optional<SSTable>> build(
		std::uint32_t table_id,
		MemTable& mem_table,
		std::filesystem::path& path,
		std::filesystem::path& final_path
	);
	static Result<std::optional<SSTable>> build(
		std::uint32_t table_id,
		SSTableIterator& it,
		std::filesystem::path& path,
		std::filesystem::path& final_path
	);

private:

	static Result<std::optional<SSTable>> build_impl(
		std::uint32_t table_id,
		const std::vector<InternalRecord>& records,
		std::filesystem::path& path,
		std::filesystem::path& final_path
	);

};