/**
 * @file kvdb.h
 * @brief Public interface for opening and operating a KVDB database.
 */
#pragma once
#include "status.h"
#include <memory>
#include <optional>
#include "db_options.h"

/**
 * @brief Abstract, thread-safe interface to the LSM key-value database.
 *
 * Mutations are ordered through the write-ahead log and MemTable. Reads search
 * the in-memory generations before consulting SSTables. Use open() instead of
 * constructing an implementation directly.
 */
class KVDB
{
public:
    virtual ~KVDB() = default;

    /**
     * @brief Validates options, creates the engine, and performs recovery.
     * @param options Database location and storage-engine policy.
     * @return An opened database or the validation/recovery error.
     * @callgraph
     */
    static Result<std::unique_ptr<KVDB>> open(const DBOptions& options);

    /** @brief Stores @p value as the newest version of @p key. */
    virtual Status put(std::string& key, std::string& value) = 0;

    /**
     * @brief Looks up the newest visible value for @p key.
     * @return An empty optional when the key is absent or deleted.
     */
    virtual Result<std::optional<std::string>> get(std::string_view key) = 0;

    /** @brief Appends a tombstone for @p key. */
    virtual Status remove(std::string& key) = 0;

    /** @brief Flushes the active and queued MemTable generations to SSTables. */
    virtual Status flush() = 0;

    /** @brief Compacts the half-open user-key range [@p begin, @p end). */
    virtual Status compact_range(std::string_view begin,
        std::string_view end) = 0;

    /** @brief Flushes pending state and releases database resources. */
    virtual Status close() = 0;

protected:
    KVDB() = default;
};
