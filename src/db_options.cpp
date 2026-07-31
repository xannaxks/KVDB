// Validation is deliberately layered: each nested option group reports local
// errors, then DBOptions checks relationships between level-indexed vectors and
// the configured maximum level count before the Engine creates any resources.
#include "db_options.h"

#include <limits>

namespace kvdb
{
    Status DBOptions::ArenaOptions::validate() const
    {
        if (page_size == 0 || page_size > (1u << 20)) {
            return Status::invalid_argument(
                "arena page_size must be in [1, 1048576]"
            );
        }
        if (large_threshold == 0) {
            return Status::invalid_argument(
                "arena large_threshold must be greater than zero"
            );
        }
        return Status::ok();
    }

    Status DBOptions::MemTableOptions::validate() const
    {
        if (size_limit == 0) {
            return Status::invalid_argument(
                "memtable size_limit must be greater than zero"
            );
        }
        if (immutable_tables_limit == 0) {
            return Status::invalid_argument(
                "memtable immutable_tables_limit must be greater than zero"
            );
        }
        return Status::ok();
    }

    Status DBOptions::WALOptions::validate() const
    {
        if (file_size_limit == 0) {
            return Status::invalid_argument(
                "WAL file_size_limit must be greater than zero"
            );
        }
        return Status::ok();
    }

    Status DBOptions::SSTableOptions::BloomFilterOptions::validate() const
    {
        if (hash_count == 0) {
            return Status::invalid_argument(
                "Bloom filter hash_count must be greater than zero"
            );
        }
        if (mask_bit_size == 0 || mask_bit_size % 8 != 0) {
            return Status::invalid_argument(
                "Bloom filter mask_bit_size must be a non-zero multiple of 8"
            );
        }
        return Status::ok();
    }

    Status DBOptions::SSTableOptions::validate() const
    {
        return bloom_filter.validate();
    }

    Status DBOptions::CompactionOptions::validate() const
    {
        if (max_levels < 2) {
            return Status::invalid_argument(
                "compaction max_levels must be at least 2"
            );
        }
        if (l0_file_count_trigger == 0 ||
            l0_file_count_trigger >
                std::numeric_limits<std::uint32_t>::max()) {
            return Status::invalid_argument(
                "compaction l0_file_count_trigger is outside the supported range"
            );
        }
        if (max_bytes_per_level.size() < max_levels ||
            target_file_size_per_level.size() < max_levels) {
            return Status::invalid_argument(
                "compaction level option arrays must cover every level"
            );
        }

        for (std::uint32_t level = 1;
            level + 1 < max_levels;
            ++level) {
            if (max_bytes_per_level[level] == 0) {
                return Status::invalid_argument(
                    "every compactable L1+ level needs a non-zero byte limit"
                );
            }
        }
        for (std::uint32_t level = 1;
            level < max_levels;
            ++level) {
            if (target_file_size_per_level[level] == 0) {
                return Status::invalid_argument(
                    "every output level needs a non-zero target file size"
                );
            }
        }
        return Status::ok();
    }

    Status DBOptions::ManifestOptions::validate() const
    {
        if (file_size_limit == 0) {
            return Status::invalid_argument(
                "manifest file_size_limit must be greater than zero"
            );
        }
        return Status::ok();
    }

    Status DBOptions::SSTableManagerOptions::validate() const
    {
        return Status::ok();
    }

    Status DBOptions::validate() const
    {
        if (db_path.empty()) {
            return Status::invalid_argument(
                "database path must not be empty"
            );
        }
        if (block_size == 0 ||
            block_size > std::numeric_limits<std::uint32_t>::max()) {
            return Status::invalid_argument(
                "block_size is outside the supported range"
            );
        }
        Status status = arena.validate();
        if (!status.is_ok()) return status;
        status = memtable.validate();
        if (!status.is_ok()) return status;
        status = wal.validate();
        if (!status.is_ok()) return status;
        status = sstable.validate();
        if (!status.is_ok()) return status;
        status = compaction.validate();
        if (!status.is_ok()) return status;
        status = manifest.validate();
        if (!status.is_ok()) return status;
        return sstable_manager.validate();
    }
}
