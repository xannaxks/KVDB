#include "sstable_loader.h"

Result<SSTable> SSTableLoader::load(
    const std::filesystem::path& path,
    Arena& arena
)
{
    return SSTable::load(path, arena);
}
