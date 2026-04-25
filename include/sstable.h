#include "mem_table.h"
#include <fstream>
#include <string>
#include <format>
#include "record.h"
#include <memory>
#include <algorithm>
#include "MurmurHash3.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <optional>
//#include "C:\Users\user\KVDB\third_party\zlib\zlib.h"
#include <bitset>

void ensure_fits_in_block(uint64_t& offset, std::size_t size, const uint32_t BLOCK_SIZE, std::ofstream& file);
void write_to_stream(std::ofstream& file, uint64_t& offset, void* ptr, std::size_t size, const uint32_t BLOCK_SIZE);
void align_to_block_boundary(uint64_t& offset, const uint32_t BLOCK_SIZE, std::ofstream& file);
template <typename T>
inline void crc32_add_pod(uint32_t& crc, const T& value)
{
	compute_crc32(crc, &value, sizeof(T));
}

inline void compute_crc32(uint32_t& crc, const void* ptr, std::size_t size);
inline uint32_t crc32_of(const void* ptr, std::size_t size);

namespace SSTableEntities {

	constexpr uint32_t FILE_HEADER_MAGIC = 0x53535431; // SST1
	constexpr uint32_t FILE_FOOTER_MAGIC = 0x46545231; // FTR1
	constexpr uint32_t SSTABLE_VERSION = 1;
	constexpr uint32_t BLOCK_SIZE = 4096;
	constexpr uint32_t BLOOM_HASH_COUNT = 2;
	
	struct FileHeaderSection;
	struct DataSection;
	struct IndexSection;
	struct BloomSection;
	struct MetaSection;
	struct FileFooterSection;

	enum class Status : uint8_t
	{
		Corrupted = 1,
	};
	enum class BlockType : uint8_t
	{
		Data = 1, 
		Index = 2, 
		Bloom = 3,
		Meta = 4
	};

	struct FileHeaderSection
	{
		uint32_t magic;
		uint32_t version;
		uint32_t flags;
		uint32_t block_size;
		uint32_t table_id;
		uint32_t crc32;

		static std::size_t disk_size();

		void write(std::ofstream& file, uint64_t& offset);
		static std::optional<FileHeaderSection> load(std::ifstream& file);

		FileHeaderSection() = default;
		FileHeaderSection(uint32_t table_id);
	};
	struct DataSection
	{
		struct Header {
			BlockType type;
			uint32_t payload_disk_size;
			uint32_t crc32;

			static std::size_t disk_size();

			Header();
		};
		struct Payload {
			uint32_t key_size;
			uint32_t value_size;
			::Type type;
			uint32_t flags;
			uint32_t reserved;
			uint64_t seq_num;
			void* key_ptr;
			void* value_ptr;

			std::size_t disk_size();
			std::size_t disk_size() const;

			static std::size_t fixed_part_disk_size();
		};
		struct DataBlock
		{
			Header header;
			std::vector<Payload> payloads;

			std::size_t disk_size() const;
			std::size_t disk_size();

			bool can_payload_fit(Payload& payload);
			void add_payload(Payload& payload);
			void write(std::ofstream& file, uint64_t& offset, IndexSection& index_section);
			static std::optional<DataBlock> load(std::ifstream& file);
		};
		std::vector<DataBlock> data_blocks;

		std::size_t disk_size();

		DataSection() = default;

		void add_payload(const InternalRecord& record);
		void write(std::ofstream& file, uint64_t& data_block_offset, IndexSection& index_section, uint64_t& data_offset);
		static std::optional<DataSection> load(std::ifstream& file, uint64_t first_data_block_offset, uint32_t data_block_count);
		void init_new_block();
	};
	struct IndexSection
	{
		struct Header {
			BlockType type;
			uint32_t payload_size;
			uint32_t crc32;

			static std::size_t disk_size();
		};
		struct Payload {
			uint64_t data_block_offset;
			uint32_t first_key_size;
			uint32_t last_key_size;
			void* first_key_ptr;
			void* last_key_ptr;

			std::size_t disk_size();
		};
		Header header;
		std::vector<Payload> payloads;

		std::size_t disk_size();

		IndexSection();

		//void rebuild(std::vector<std::tuple<DataSection::Payload*, DataSection::Payload*, uint64_t, uint64_t>>& index_boundaries);
		void add_index(uint64_t data_block_offset, uint32_t first_key_size, uint32_t last_key_size, void* first_key_ptr, void* last_key_ptr);
		void write(std::ofstream& file, uint64_t& offset, uint64_t& index_offset);
		static std::optional<IndexSection> load(std::ifstream& file, Arena& arena, uint64_t index_offset = 0);
	};
	struct BloomSection
	{
		struct Header {
			BlockType type;
			uint32_t payload_size;
			uint32_t crc32;

			static std::size_t disk_size();
		};
		struct Payload {
			uint64_t bloom_bits;
			uint32_t hash_count;
			uint32_t key_count;
			std::bitset<128> mask;

			static std::size_t disk_size();
		};
		Header header;
		Payload payload;

		static std::size_t disk_size();

		BloomSection();

		//void rebuild(DataSection& data_block);
		void write(std::ofstream& file, uint64_t& offset, uint64_t& bloom_offset);
		static std::optional<BloomSection> load(std::ifstream& file, uint64_t bloom_offset = 0);
		void add_key(const void* key_ptr, uint32_t key_size);
		bool may_contain(const void* key_ptr, uint32_t key_size) const;
		void rebuild(const DataSection& data_section);
		void recompute_crc32();
	};
	struct MetaSection
	{
		struct Header {
			BlockType type;
			uint32_t payload_size;
			uint32_t crc32;

			static std::size_t disk_size();
		};
		struct Payload {
			uint64_t record_count;
			uint64_t tombstone_count;
			uint64_t min_seq_num;
			uint64_t max_seq_num;

			uint32_t min_key_size;	
			uint32_t max_key_size;

			uint64_t data_block_count;
			uint64_t data_bytes;
		
			static std::size_t disk_size();
		};

		Header header;
		Payload payload;

		static std::size_t disk_size();

		MetaSection();

		//void rebuild(DataSection& data_block, IndexSection& index_block);
		void write(std::ofstream& file, uint64_t& offset, uint64_t& meta_offset);
		static std::optional<MetaSection> load(
			std::ifstream& file,
			DataSection& data_block,
			IndexSection& index_block,
			uint64_t meta_offset = 0
		);
		void rebuild(DataSection& data_section, IndexSection& index_section);
	};
	struct FileFooterSection
	{
		uint32_t magic;
		uint32_t version;
		uint32_t reserved;

		uint64_t data_offset;
		uint32_t data_block_count;

		uint64_t index_offset;
		uint32_t index_size;

		uint64_t bloom_offset;
		uint32_t bloom_size;

		uint64_t meta_offset;
		uint32_t meta_size;

		uint64_t file_size;
		uint32_t footer_crc32;

		static std::size_t disk_size();

		FileFooterSection();
		
		void finalize(uint64_t offset);
		void rebuild(IndexSection& index_block, uint64_t index_offset);
		void rebuild(BloomSection& bloom_block, uint64_t bloom_offset);
		void rebuild(MetaSection& meta_block, uint64_t meta_offset);
		void write(std::ofstream& file, uint64_t& offset);
		static std::optional<FileFooterSection> load(
			std::ifstream& file,
			uint64_t file_footer_offset = 0,
			auto dir = std::ios::beg
		);
	};
}

class SSTable
{
public:
	SSTable(const std::string& path);
	//SSTable(const MemTable& mem_table, uint32_t id);

private:
	std::string path;
	std::ifstream file_in;
	std::ofstream file_out;
	SSTableEntities::FileHeaderSection file_header_section{};
	SSTableEntities::DataSection data_section{};
	SSTableEntities::IndexSection index_section{};
	SSTableEntities::BloomSection bloom_section{};
	SSTableEntities::MetaSection meta_section{};
	SSTableEntities::FileFooterSection file_footer_section{};

	void write();

	friend class SSTableManager;
	friend class SSTableWriter;
	friend class SSTableLoader;
};

class SSTableWriter
{
public:
	SSTableWriter();

	static void write(SSTable& sstable);
};
class SSTableLoader
{
public:
	SSTableLoader();

	static std::optional<SSTable> load(const std::string& path, Arena& arena);
};

class SSTableManager
{
private:
	SSTableWriter sstable_writer;
	SSTableLoader sstable_loader;
	
	std::vector<SSTable> immutable_pool;
	std::vector<SSTable> pool;

public:
	SSTableManager();
	void write_latest(bool erase = false);
	void write_all(bool erase = false);
	void add_to_pool(SSTable& sstable);
	//void add_to_pool(MemTable& mem_table);
	void load(Arena& arena);
	//void get_latest();
};
