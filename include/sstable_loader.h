#include "status.h"
#include "sstable.h"
#include <filesystem>
#include "arena.h"
#include "sstable_entities.h"

class SSTableLoader
{
public:
	SSTableLoader() noexcept = default;

	static Result<SSTable> load( std::filesystem::path& path, Arena& arena);
};
