/**
 * @file type.h
 * @brief Mutation kind stored in internal records.
 */
#pragma once

#include <cstdint>

/**
 * @brief Logical state carried by a versioned record.
 *
 * Tombstones stop older-value lookup and may be discarded only when compaction
 * proves that no lower level can still contain the deleted key.
 */
enum class Type : uint8_t
{
    Put = 0, ///< Key has a stored value.
    Tombstone = 1, ///< Key is logically deleted at this sequence.
    Undefined ///< Invalid or uninitialized record type.
};
