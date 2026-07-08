#include "sstable_loader.h"

Result<SSTable> SSTableLoader::load( std::filesystem::path& path, Arena& arena)
{
    return SSTable::load(path, arena);
}
