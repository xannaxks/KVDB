#pragma once

#include "arena.h"
#include "sstable.h"
#include "status.h"

#include <filesystem>

class SSTableLoader
{
public:
    SSTableLoader() noexcept = default;

    [[nodiscard]] static Result<SSTable> load(
        const std::filesystem::path& path,
        Arena& arena
    );
};
