/**
 * @file sstable_iterator.h
 * @brief Forward record cursor over a validated SSTable data section.
 */
#pragma once

#include "arena.h"
#include "status.h"
#include "sstable.h"
#include "record.h"
#include "file.h"
#include "file_helpers.h"

#include <cstdint>
#include <memory>

/**
 * @brief Lazily reads SSTable data blocks in internal-key order.
 *
 * Only the current block's records are materialized. Their key/value bytes are
 * copied into the supplied Arena and therefore follow its lifetime.
 */
class SSTableIterator
{
private:
	const SSTable& sstable;
	std::unique_ptr<ReadableFile> file;
	Arena& arena;

	std::uint64_t current_offset = 0;

	std::uint64_t data_block_count = 0;
	std::uint64_t next_block_index = 0;

	std::size_t record_index = 0;
	std::vector<InternalRecord> current_block_records;

	bool valid_ = false;
	Status status_ = Status::ok();

public:
	SSTableIterator() = delete;
	SSTableIterator(const SSTable& sstable, std::unique_ptr<ReadableFile>&& file, Arena& arena);

	/** @brief Positions at the first record, or becomes invalid for an empty table. */
	Status seek_to_first();
	/** @brief Advances to the next record, loading a new block when necessary. */
	Status next();

	bool valid() const;
	const InternalRecord& record() const;
	Status status() const;

private:
	Status load_next_block();
};
