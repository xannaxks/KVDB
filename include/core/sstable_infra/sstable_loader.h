/**
 * @file sstable_loader.h
 * @brief Convenience entry point for validated SSTable loading.
 */
#pragma once

#include "core/allocator/arena.h"
#include "core/engine/sstable.h"
#include "utils/status.h"

#include <filesystem>

/** @brief Stateless facade around SSTable::load(). */
class SSTableLoader
{
public:
    SSTableLoader() noexcept = default;

    [[nodiscard]] static Result<SSTable> load(
        const std::filesystem::path& path,
        Arena& arena
    );
};
