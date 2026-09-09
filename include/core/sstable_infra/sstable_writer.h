/**
 * @file sstable_writer.h
 * @brief Convenience entry point for publishing a built SSTable.
 */
#pragma once

#include "core/engine/sstable.h"
#include "utils/status.h"

/** @brief Stateless facade around SSTable::write(). */
class SSTableWriter
{
public:
    [[nodiscard]] static Status write(SSTable& sstable);
};
