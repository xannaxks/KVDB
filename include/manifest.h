#include <algorithm>
#include <fstream>
#include <cstdint>
#include <vector>
#include <string>
#include <optional>
#include <bitset>
#include <utility>
#include <cstddef>
#include <cassert>
#include <unordered_set>
#include "wal.h"
#include "sstable.h"

struct DeletedTable
{
	uint32_t level = 0;
	uint64_t table_id = 0;

	static DeletedTable load(std::ifstream& file);
	void write(std::ofstream& file);

	uint32_t disk_size() const;
};

struct SSTableMeta
{
	uint64_t table_id;
	uint64_t file_size;

	uint64_t smallest_seq;
	uint64_t largest_seq;

	std::vector<std::byte> smallest_key;
	std::vector<std::byte> largest_key;

	uint64_t record_count = 0;
	uint64_t tombstone_count;

	uint32_t level = 0;

	static SSTableMeta load(std::ifstream& file);
	void write(std::ofstream& file);

	uint32_t disk_size() const;
};

struct VersionEdit
{

	struct Header
	{
		uint32_t crc32;
		uint32_t payload_size;

		void compute_crc32(VersionEdit& version_edit);
		void compute_payload_size(VersionEdit& version_edit);
			
		static VersionEdit::Header load(std::ifstream& file);
		void write(std::ofstream& file);
	};

	uint64_t next_table_id;
	uint64_t last_seq_num;
	uint64_t current_wal_id;

	std::vector<SSTableMeta> new_tables;
	std::vector<DeletedTable> deleted_tables;

	static VersionEdit load(std::ifstream& file);
	void write(std::ofstream& file);

	uint32_t disk_size();
};

class Manifest
{
public:
	struct Header
	{
		uint32_t magic = MANIFEST_MAGIC; // MANI
		uint32_t version = MANIFEST_VERSION;
		uint32_t block_size = MANIFEST_BLOCK_SIZE;
		uint32_t header_size = Header::disk_size();
		uint32_t reserved = 0;
		uint32_t crc32 = 0;

		static uint32_t disk_size();
		void compute_crc32();

		void write(std::ofstream& file);
		static Header load(std::ifstream& file);
	}

private:
	uint64_t next_table_id = 0;
	uint64_t last_sequence_number = 0;
	uint64_t current_wal_id = 1;

	std::vector<std::vector<SSTableMeta>> levels;

public:
	uint64_t get_next_table_id()
	{
		return this->next_table_id ++;
	}
	uint64_t get_current_wal_id() const
	{
		return this->current_wal_id;
	}
	uint64_t get_last_sequence_number() const
	{
		return this->last_sequence_number;
	}
	const std::vector<SSTableMeta>& get_level(uint32_t level) const
	{
		return this->levels.at(level);
	}
	const std::vector<std::vector<SSTableMeta>>& get_levels() const
	{
		return this->levels;
	}

	void write(std::ifstream& file);
	static Manifest load(std::ofstream& file);

	bool apply(const VersionEdit& edit)
	{
		if (edit.next_table_id)
		{
			if (*(edit.next_table_id) <= this->next_table_id)
				return false;
			this->next_table_id = *(edit.next_table_id);
		}
		if (edit.last_seq_number)
		{
			if (*(edit.last_seq_number) < this->last_seq_number)
				return false;

			this->last_seq_number = *(edit.last_seq_number);
		}
		if (edit.current_wal_id)
		{
			if (this->current_wal_id > *(edit.current_wal_id))
				return false;
			this->current_wal_id = *(edit.current_wal_id);
		}

		for (const auto& deleted : edit.deleted_tables)
		{
			if (deleted.level >= this->levels.size())
				return false;

			auto& tables = this->levels[deleted.level];

			tables.erase(
				std::remove_if(
					tables.begin(), 
					tables.end(), 
					[&](const SSTableMeta& table)
					{
						return table.table_id == deleted.table_id;
					}
				),
				tables.end()
			);
		}

		for (const auto& new_table : edit.new_tables)
		{
			if (new_table.level >= this->levels.size()) return false;
			if (!valid_table_meta(new_table)) return false;

			auto& tables = this->levels[new_table.level];
			tables.emplace_back(new_table);
		}

		for (auto& table : this->levels)
		{
			std::sort(
				table.begin(),
				table.end(),
				[](const SSTableMeta& a, const SSTableMeta& b) {
					return a.smallest_key < b.smallest_key;
				}
			);
		}

		return check_invariants();
	}

private:
	static bool valid_table_meta(const SSTableMeta& table)
	{
		if (table.table_id == 0) return false;
		if (table.smallest_key > table.largest_key) return false;
		if (table.smallest_seq > table.largest_seq) return false;
		return true;
	}

	bool check_invariants() const
	{
		std::unordered_set <uint64_t> seen_ids;
		
		for (std::size_t level = 0; level < levels.size(); level++)
		{
			const auto& tables = levels[level];

			for (const auto& table : tables)
			{
				if (table.level != level) return false;
				if (!seen_ids.insert(table.table_id).second) return false;
			}  /// checking for duplicate wal_ids

			if (level >= 1)
			{
				for (std::size_t i = 1; i < tables.size(); i++)
				{
					const auto& prev = tables[i - 1], current = tables[i];
				
					if (prev.largest_key >= current.smallest_key)
						return false;
				}
			} // checking for overlapping keys for sstables with level >= 1

		}

		return true;
	}
};