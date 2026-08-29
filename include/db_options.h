/**
 * @file db_options.h
 * @brief Configuration and cross-field validation for the storage engine.
 */
#pragma once

#include "status.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

class RBTree;
class SkipList;

#if defined(KVDB_DRIVER_RBTREE)
using MemTableDriver = RBTree;
#elif defined(KVDB_DRIVER_SKIPLIST)
using MemTableDriver = SkipList;
#else
#error "No Memtable driver defined. Please define either RBTree or SkipList." 
#endif

namespace kvdb {

    /**
     * @brief Complete configuration used to create or recover a database.
     *
     * Each nested component validates its local constraints; validate() also
     * checks cross-component requirements such as level-vector lengths.
     */
    struct DBOptions
    {
        // -------------------------------------------------------------------------
        // General database
        // -------------------------------------------------------------------------

        std::filesystem::path db_path;

        bool create_if_missing = true;
        bool error_if_exists = false;

        // Common block size used by storage components where applicable.
        std::size_t block_size = 4 * 1024;

        // -------------------------------------------------------------------------
        // Arena
        // -------------------------------------------------------------------------

        /** @brief Arena page sizing and large-allocation threshold. */
        struct ArenaOptions
        {
            std::size_t page_size = 64 * 1024;
            std::size_t large_threshold = 16 * 1024;

            [[nodiscard]] Status validate() const;
        };

        ArenaOptions arena{};

        // -------------------------------------------------------------------------
        // MemTable
        // -------------------------------------------------------------------------

        /** @brief Flush threshold and immutable-generation backpressure. */
        struct MemTableOptions
        {
            // Flush active MemTable after approximately this many bytes.
            std::size_t size_limit = 64 * 1024 * 1024;

            // Number of immutable MemTables allowed to wait for flushing.
            std::size_t immutable_tables_limit = 1;

            [[nodiscard]] Status validate() const;
        };

        MemTableOptions memtable{};

        // -------------------------------------------------------------------------
        // WAL
        // -------------------------------------------------------------------------

        /** @brief WAL rotation limit and per-write durability policy. */
        struct WALOptions
        {
            // Maximum size of one WAL file before rotation.
            std::uint64_t file_size_limit = 64ull * 1024 * 1024;

            // Whether write operations should fsync before reporting success.
            bool sync_on_write = false;

            [[nodiscard]] Status validate() const;
        };

        WALOptions wal{};

        // -------------------------------------------------------------------------
        // SSTable
        // -------------------------------------------------------------------------

        /** @brief Immutable-table format options. */
        struct SSTableOptions
        {
            /** @brief Bloom-filter hash and storage sizing. */
            struct BloomFilterOptions
            {
                std::uint32_t hash_count = 2;

                // Current design uses a fixed-size bit mask.
                std::uint32_t mask_bit_size = 128;

                [[nodiscard]] Status validate() const;
            };

            BloomFilterOptions bloom_filter{};

            [[nodiscard]] Status validate() const;
        };

        SSTableOptions sstable{};

        // -------------------------------------------------------------------------
        // Compaction
        // -------------------------------------------------------------------------

        /** @brief Level count, pressure thresholds, and output table sizing. */
        struct CompactionOptions
        {
            bool enable_background_compaction = true;

            std::uint32_t max_levels = 7;

            // L0 files may overlap, so file count is used as its primary trigger.
            std::size_t l0_file_count_trigger = 4;

            // Maximum total bytes allowed at each level.
            //
            // L0 uses its file-count trigger instead.
            // The bottommost level has no further destination level, so 0 means
            // "no size-triggered compaction".
            std::vector<std::uint64_t> max_bytes_per_level = {
                0,
                64ull * 1024 * 1024,
                640ull * 1024 * 1024,
                6400ull * 1024 * 1024,
                64000ull * 1024 * 1024,
                640000ull * 1024 * 1024,
                0
            };

            // Desired size of newly produced SSTables at each level.
            std::vector<std::uint64_t> target_file_size_per_level = {
                8ull * 1024 * 1024,
                8ull * 1024 * 1024,
                16ull * 1024 * 1024,
                32ull * 1024 * 1024,
                64ull * 1024 * 1024,
                128ull * 1024 * 1024,
                256ull * 1024 * 1024
            };

            [[nodiscard]] Status validate() const;
        };

        CompactionOptions compaction{};

        // -------------------------------------------------------------------------
        // Manifest
        // -------------------------------------------------------------------------

        /** @brief Threshold for future manifest rewriting/compaction. */
        struct ManifestOptions
        {
            // Rewrite/compact the manifest once it grows beyond this size.
            std::uint64_t file_size_limit = 16ull * 1024 * 1024;

            [[nodiscard]] Status validate() const;
        };

        ManifestOptions manifest{};

        // -------------------------------------------------------------------------
        // SSTable Manager
        // -------------------------------------------------------------------------

        /** @brief SSTable caching and loading policy. */
        struct SSTableManagerOptions
        {
            // If true, SSTables are loaded/opened only when needed instead of
            // keeping every table open.
            bool lazy_loading = true;

            [[nodiscard]] Status validate() const;
        };

        SSTableManagerOptions sstable_manager{};

        // -------------------------------------------------------------------------
        // Validation
        // -------------------------------------------------------------------------

        /** @brief Validates every component and their cross-field invariants. */
        [[nodiscard]] Status validate() const;
    };

} // namespace kvdb

using DBOptions = kvdb::DBOptions;
