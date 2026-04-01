#include "mem_table.h"
#include <fstream>
#include <string>
#include <format>
#include "record.h"
#include <memory>
#include "MurmurHash3.h"

namespace SSTableEntities {
	struct Data
	{
		uint32_t key_size, value_size;
		Type type;
		uint64_t seq_num;
		Bytes key, value;

		Data(uint32_t key_size, const Bytes& key, uint32_t value_size, const Bytes& value, Type type, uint64_t seq_num);
		Data(const InternalRecord& record);
		Data() = default;

		void write(std::ofstream& file, int& in_block_offset, uint32_t& block_offset) const;
		uint32_t get_size() const;

		static Data read(std::ifstream& file);
	};
	struct Index
	{
		uint32_t key_size;
		uint32_t block_offset;
		uint32_t data_coverage;
		Bytes key;

		Index(uint32_t key_size, const Bytes& key, uint32_t block_offset, uint32_t data_coverage);
		Index(const Data& data, uint32_t block_offset, uint32_t data_coverage);
		Index() = default;

		void write(std::ofstream& file, int& in_block_offset, uint32_t& block_offset) const;
		uint32_t get_size() const;

		static Index read(std::ifstream& file);
	};
	struct Footer
	{
		uint32_t magic;
		int in_block_offset;
		uint32_t block_offset;
		uint32_t index_count;

		Footer(int in_block_offset, uint32_t block_offset, uint32_t index_count);
		Footer() = default;

		void write(std::ofstream& file, int& in_block_offset, uint32_t& block_offset) const;
		uint32_t get_size() const;

		static Footer read(std::ifstream& file);
	};
	struct SSTableBloomFilter
	{
		static uint32_t bloom_bits;
		std::unique_ptr<uint8_t[]> bloom_ptr;

		SSTableBloomFilter();

		void add_key(const Bytes& key);
		bool may_contain(const Bytes& key) const;

		void write(std::ofstream& file, int& in_block_offset, uint32_t& block_offset) const;
		static SSTableBloomFilter read(std::ifstream& file);

		static uint32_t get_storage_size();
		static uint32_t get_hash1(const Bytes& key);
		static uint32_t get_hash2(const Bytes& key);

	private:
		std::vector<uint32_t> get_hashes(const Bytes& key, int hash_amount = 3) const;
		void set_bit(uint32_t bit_index);
		bool test_bit(uint32_t bit_index) const;
	};

	constexpr uint32_t SSTableMagic = 0xBADCAFFE;
	constexpr int BLOCK_SIZE = 4096;
	
	void next_block(std::ofstream& file, int& in_block_offset, uint32_t& block_offset);
	void ensure_block(std::ofstream& file, int& in_block_offset, uint32_t& block_offset, uint32_t size);

	std::string construct_sstable_name(const std::string& sstable_dir, uint32_t sstable_num);

	constexpr uint32_t seed1 = 0x9747b28c;
	constexpr uint32_t seed2 = 0x85ebca6b;
}

class SSTable
{
public:
	enum class Status
	{
		KeyNotFound,
		Corrupted,
		KeyWasDeleted
	};

	SSTable(const std::string& path);

	std::variant<ByteRecord, SSTable::Status> get(const Bytes& key);

private:
	std::string path;
	std::ifstream file;
	std::vector<SSTableEntities::Index> indexes;
	SSTableEntities::Footer footer{};

	void load_footer();
	void load_indexes();
};

class SSTableBuilder
{
public:
	static void build_from_records(const std::vector<InternalRecord>& records, const std::string& path);
	static void build_from_memtable(MemTable& mem_table, const std::string& sstable_dir, uint32_t& sstable_num);
};