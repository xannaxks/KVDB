/**
 * @file sstable_writer.h
 * @brief Convenience entry point for publishing a built SSTable.
 */
#pragma once

#include "sstable.h"
#include "status.h"

/** @brief Stateless facade around SSTable::write(). */
class SSTableWriter
{
public:
    [[nodiscard]] static Status write(SSTable& sstable);
};
