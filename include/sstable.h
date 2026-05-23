#ifdef _WIN32

	#ifndef NOMINMAX
		#define NOMINMAX
	#endif

	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif

#endif

#include "mem_table.h"
#include <fstream>
#include <string>
#include <format>
#include "record.h"
#include <memory>
#include <algorithm>
#include "MurmurHash3.h"
#include <filesystem>
#include <algorithm>
#include <limits>
#include <format>
#include "file.h"
#include <stdexcept>
#include <optional>
#include "file_helpers.h"
#include "crc32_helpers.h"
//#include "zlib.h"
#include <bitset>
#include <cstdint>
#include "endian_io.h"

// loading correction

void ensure_atomicity();

void do_fsync();


namespace SSTableEntities {

	constexpr std::uint32_t FILE_HEADER_MAGIC = 0x53535431; // SST1
	constexpr std::uint32_t FILE_FOOTER_MAGIC = 0x46545231; // FTR1
	constexpr std::uint32_t SSTABLE_VERSION = 1;
	constexpr std::uint32_t BLOCK_SIZE = 4096;
	constexpr std::uint32_t BLOOM_HASH_COUNT = 2;
	constexpr std::uint32_t BLOOM_MASK_BIT_SIZE = 128; // amount of bits in bloom mask

	struct FileHeaderSection;
	struct DataSection;
	struct DataSectionView;
	struct IndexSection;
	struct BloomSection;
	struct MetaSection;
	struct FileFooterSection;

	enum class Status : std::uint8_t
	{
		Corrupted = 1,
	};
	enum class BlockType : std::uint8_t
	{
		Data = 1,
		Index = 2,
		Bloom = 3,
		Meta = 4
	};

	struct FileHeaderSection
	{
		FileHeaderSection() = default;
		FileHeaderSection(std::uint32_t table_id);
		FileHeaderSection(const FileHeaderSection& other) noexcept = default;
		FileHeaderSection(FileHeaderSection& other) noexcept = default;
		FileHeaderSection(FileHeaderSection&& other) noexcept = default;

		std::uint32_t magic;
		std::uint32_t version;
		std::uint32_t flags;
		std::uint32_t block_size;
		std::uint32_t table_id;
		std::uint32_t crc32;

		static std::size_t disk_size();

		void write(WritableFile& file, std::uint64_t& offset);
		static std::optional<FileHeaderSection> load(ReadableFile& file, std::uint64_t& offset);

		void calculate_crc32(std::uint32_t& crc_buffer);

		FileHeaderSection& operator=(FileHeaderSection& other) = default;
		FileHeaderSection& operator=(FileHeaderSection&& other) = default;
	};
	struct DataSection
	{
		struct Header {
			Header();
			Header(const Header& other) noexcept = default;
			Header(Header& other) noexcept = default;
			Header(Header&& other) noexcept = default;

			BlockType type;
			std::uint32_t payload_disk_size;
			std::uint32_t crc32;

			static std::size_t disk_size();

			//static std::optional<Header> load(std::unique_ptr<ReadableFile>* file, std::uint64_t& offset);
			bool write(WritableFile& file, std::uint64_t& offset);

			Header& operator=(Header& other) = default;
			Header& operator=(Header&& other) = default;
		};

		struct Payload {
			Payload() noexcept = default;
			Payload(const Payload& other) noexcept = default;
			Payload(Payload& other) noexcept = default;
			Payload(Payload&& other) noexcept = default;

			std::uint32_t key_size;
			std::uint32_t value_size;
			::Type type;
			std::uint32_t flags;
			std::uint32_t reserved;
			std::uint64_t seq_num;
			void* key_ptr;
			void* value_ptr;

			std::size_t disk_size();
			std::size_t disk_size() const;

			static std::optional<Payload> load(ReadableFile& file, std::uint64_t& offset, std::uint32_t* must_be_crc = nullptr);
			bool write(WritableFile& file, std::uint64_t& offset);

			static std::size_t fixed_part_disk_size();

			void calculate_crc32(std::uint32_t& crc_buffer);
			void append_crc32(std::uint32_t& crc_buffer);

			Payload& operator=(Payload& other) = default;
			Payload& operator=(Payload&& other) = default;
		};
		struct DataBlock
		{
			DataBlock() noexcept = default;
			DataBlock(const DataBlock& other) noexcept = default;
			DataBlock(DataBlock& other) noexcept = default;
			DataBlock(DataBlock&& other) noexcept = default;

			Header header;
			std::vector<Payload> payloads;

			std::size_t disk_size() const;
			std::size_t disk_size();

			bool can_payload_fit(Payload& payload);
			void add_payload(Payload& payload);

			bool write(WritableFile& file, std::uint64_t& offset, IndexSection& index_section);
			//static std::optional<DataBlock> load(std::unique_ptr<ReadableFile>* file, std::uint64_t& offset);

			void calculate_crc32(std::uint32_t& crc_buffer);

			DataBlock& operator=(DataBlock& other) = default;
			DataBlock& operator=(DataBlock&& other) = default;
		};
		DataSection() noexcept = default;
		DataSection(const DataSection& other) noexcept = default;
		DataSection(DataSection& other) noexcept = default;
		DataSection(DataSection&& other) noexcept = default;

		std::vector<DataBlock> data_blocks;

		void init_new_block();
		void add_payload(const InternalRecord& record);
		
		std::size_t disk_size();

		void write(WritableFile& file, std::uint64_t& data_block_offset, IndexSection& index_section, std::uint64_t& data_offset);
		//static std::optional<DataSection> load(
		//	std::unique_ptr<ReadableFile>* file,
		//	std::uint64_t& offset,
		//	const std::uint64_t& first_data_block_offset,
		//	std::uint32_t data_block_count
		//);

		DataSection& operator=(DataSection& other) = default;
		DataSection& operator=(DataSection&& other) = default;
	};

	struct DataSectionView
	{
		struct Header
		{
			Header();
			Header(const Header& other) noexcept = default;
			Header(Header& other) noexcept = default;
			Header(Header&& other) noexcept = default;

			DataSection::Header header{};

			std::uint64_t header_offset{};
			std::uint64_t payload_offset{};
			std::uint64_t next_block_offset{};

			static std::optional<Header> load(ReadableFile& file, std::uint64_t& offset);

			Header& operator=(Header& other) = default;
			Header& operator=(Header&& other) = default;
		};
		struct DataBlock
		{
			DataBlock() noexcept = default;
			DataBlock(const DataBlock& other) noexcept = default;
			DataBlock(DataBlock& other) noexcept = default;
			DataBlock(DataBlock&& other) noexcept = default;

			Header header_view{};

			static std::optional<DataBlock> load(ReadableFile& file, std::uint64_t& offset);

			DataBlock& operator=(DataBlock& other) = default;
			DataBlock& operator=(DataBlock&& other) = default;
		};

		DataSectionView() noexcept = default;
		DataSectionView(const DataSectionView& other) noexcept = default;
		DataSectionView(DataSectionView& other) noexcept = default;
		DataSectionView(DataSectionView&& other) noexcept = default;

		std::vector<DataBlock> data_blocks{};

		static std::optional<DataSectionView> load(
			ReadableFile& file,
			std::uint64_t& offset,
			const std::uint64_t& first_data_block_offset,
			std::uint32_t data_block_count
		);

		DataSectionView& operator=(DataSectionView& other) = default;
		DataSectionView& operator=(DataSectionView&& other) = default;
	};

	struct IndexSection
	{
		struct Header {
			Header() = default;
			Header(const Header& other) noexcept = default;
			Header(Header& other) noexcept = default;
			Header(Header&& other) noexcept = default;

			BlockType type;
			std::uint32_t payload_size;
			std::uint32_t crc32;

			bool write(WritableFile& file, std::uint64_t& offset);
			static std::optional<Header> load(ReadableFile& file, std::uint64_t& offset);

			static std::size_t disk_size();

			Header& operator=(Header& other) = default;
			Header& operator=(Header&& other) = default;
		};
		struct Payload {
			Payload() = default;
			Payload(const Payload& other) noexcept = default;
			Payload(Payload& other) noexcept = default;
			Payload(Payload&& other) noexcept = default;
			
			std::uint64_t data_block_offset;
			std::uint32_t first_key_size;
			std::uint32_t last_key_size;
			void* first_key_ptr;
			void* last_key_ptr;

			bool write(WritableFile& file, std::uint64_t& offset);
			static std::optional<Payload> load(ReadableFile& file, std::uint64_t& offset, Arena& arena);

			std::size_t disk_size();
			static std::size_t fixed_disk_size();

			void calculate_crc32(std::uint32_t& crc_buffer);
			void append_crc32(std::uint32_t& crc_buffer);

			Payload& operator=(Payload& other) = default;
			Payload& operator=(Payload&& other) = default;
		};
		IndexSection();
		IndexSection(const IndexSection& other) noexcept = default;
		IndexSection(IndexSection& other) noexcept = default;
		IndexSection(IndexSection&& other) noexcept = default;

		Header header;
		std::vector<Payload> payloads;

		std::size_t disk_size();

		//void rebuild(std::vector<std::tuple<DataSection::Payload*, DataSection::Payload*, std::uint64_t, std::uint64_t>>& index_boundaries);
		void add_index(std::uint64_t data_block_offset, std::uint32_t first_key_size, std::uint32_t last_key_size, void* first_key_ptr, void* last_key_ptr);
		
		void write(WritableFile& file, std::uint64_t& offset, std::uint64_t& index_offset);
		static std::optional<IndexSection> load(
			ReadableFile& file,
			std::uint64_t& offset,
			Arena& arena,
			const std::uint64_t& index_offset
		);

		void calculate_crc32(std::uint32_t& crc_buffer);

		IndexSection& operator=(IndexSection& other) = default;
		IndexSection& operator=(IndexSection&& other) = default;
	};
	struct BloomSection
	{
		struct Header {
			Header() = default;
			Header(const Header& other) noexcept = default;
			Header(Header& other) noexcept = default;
			Header(Header&& other) noexcept = default;
			
			BlockType type; // std::uint8_t
			std::uint32_t payload_size;
			std::uint32_t crc32;

			bool write(WritableFile& file, std::uint64_t& offset);
			static std::optional<Header> load(ReadableFile& file, std::uint64_t& offset);

			static std::size_t disk_size();

			Header& operator=(Header& other) = default;
			Header& operator=(Header&& other) = default;
		};
		struct Payload {
			Payload() = default;
			Payload(const Payload& other) noexcept = default;
			Payload(Payload& other) noexcept = default;
			Payload(Payload&& other) noexcept = default;
			
			std::uint64_t bloom_bits;
			std::uint32_t hash_count;
			std::uint32_t key_count;
			std::vector<std::uint8_t> mask;

			bool write(WritableFile& file, std::uint64_t& offset);
			static std::optional<Payload> load(ReadableFile& file, std::uint64_t& offset);

			static std::size_t disk_size();

			void calculate_crc32(std::uint32_t& crc_buffer);

			Payload& operator=(Payload& other) = default;
			Payload& operator=(Payload&& other) = default;
		};
		BloomSection();
		BloomSection(const BloomSection& other) noexcept = default;
		BloomSection(BloomSection& other) noexcept = default;
		BloomSection(BloomSection&& other) noexcept = default;

		Header header;
		Payload payload;

		static std::size_t disk_size();

		//void rebuild(DataSection& data_block);
		void write(WritableFile& file, std::uint64_t& offset, std::uint64_t& bloom_offset);
		static std::optional<BloomSection> load(ReadableFile& file, std::uint64_t& offset, const std::uint64_t& bloom_offset);

		void add_key(const void* key_ptr, std::uint32_t key_size);
		void rebuild(const DataSection& data_section);
		void recompute_crc32();

		bool may_contain(const void* key_ptr, std::uint32_t key_size) const;

		void calculate_crc32(std::uint32_t& crc_buffer);

		BloomSection& operator=(BloomSection& other) = default;
		BloomSection& operator=(BloomSection&& other) = default;
	};
	struct MetaSection
	{
		struct Header {
			Header() = default;
			Header(const Header& other) noexcept = default;
			Header(Header& other) noexcept = default;
			Header(Header&& other) noexcept = default;

			BlockType type;
			std::uint32_t payload_size;
			std::uint32_t crc32;

			bool write(WritableFile& file, std::uint64_t& offset);
			static std::optional<Header> load(ReadableFile& file, std::uint64_t & offset);

			static std::size_t disk_size();
			static std::size_t fixed_disk_size();

			Header& operator=(Header& other) = default;
			Header& operator=(Header&& other) = default;
		};
		struct Payload {
			Payload() = default;
			Payload(const Payload& other) noexcept = default;
			Payload(Payload& other) noexcept = default;
			Payload(Payload&& other) noexcept = default;

			std::uint64_t record_count;
			std::uint64_t tombstone_count;
			std::uint64_t min_seq_num;
			std::uint64_t max_seq_num;

			std::uint32_t min_key_size;
			std::uint32_t max_key_size;

			std::uint64_t data_block_count;
			std::uint64_t data_bytes;

			void* max_key_ptr;
			void* min_key_ptr;

			bool write(WritableFile& file, std::uint64_t& offset);
			static std::optional<Payload> load(ReadableFile& file, std::uint64_t& offset, Arena& arena, MetaSection::Header& header);

			std::size_t disk_size();
			static std::size_t fixed_disk_size();

			void calculate_crc32(std::uint32_t& crc_buffer);

			Payload& operator=(Payload& other) = default;
			Payload& operator=(Payload&& other) = default;
		};
		MetaSection();
		MetaSection(const MetaSection& other) noexcept = default;
		MetaSection(MetaSection& other) noexcept = default;
		MetaSection(MetaSection&& other) noexcept = default;

		Header header;
		Payload payload;

		std::size_t disk_size();
		static std::size_t fixed_disk_size();

		void rebuild(DataSection& data_section, IndexSection& index_section);

		//void rebuild(DataSection& data_block, IndexSection& index_block);
		void write(WritableFile& file, std::uint64_t& offset, std::uint64_t& meta_offset);
		static std::optional<MetaSection> load(
			ReadableFile& file,
			std::uint64_t& offset,
			IndexSection& index_block,
			Arena& arena,
			const std::uint64_t& meta_offset = 0
		);

		void calculate_crc32(std::uint32_t& crc_buffer);

		MetaSection& operator=(MetaSection& other) = default;
		MetaSection& operator=(MetaSection&& other) = default;
	};
	struct FileFooterSection
	{
		FileFooterSection();
		FileFooterSection(const FileFooterSection& other) noexcept = default;
		FileFooterSection(FileFooterSection& other) noexcept = default;
		FileFooterSection(FileFooterSection&& other) noexcept = default;

		std::uint32_t magic;
		std::uint32_t version;
		std::uint32_t reserved;

		std::uint64_t data_offset;
		std::uint64_t data_block_count;

		std::uint64_t index_offset;
		std::uint32_t index_size;

		std::uint64_t bloom_offset;
		std::uint32_t bloom_size;

		std::uint64_t meta_offset;
		std::uint32_t meta_size;

		std::uint64_t file_size;
		std::uint32_t footer_crc32;

		static std::size_t disk_size();

		void finalize(WritableFile& file, std::uint64_t offset);
		static std::optional<FileFooterSection> load(ReadableFile& file, std::uint64_t& offset, std::uint64_t file_footer_backwards_offset);
		void rebuild(IndexSection& index_block, std::uint64_t index_offset);
		void rebuild(BloomSection& bloom_block, std::uint64_t bloom_offset);
		void rebuild(MetaSection& meta_block, std::uint64_t meta_offset);

		bool write(WritableFile& file, std::uint64_t& offset);
		//static std::optional<FileFooterSection> load(
		//	ReadableFile& file,
		//	std::uint64_t& offset,
		//	std::uint64_t file_footer_backwards_offset = 0,
		//);

		void calculate_crc32(std::uint32_t& crc_buffer);

		FileFooterSection& operator=(FileFooterSection& other) = default;
		FileFooterSection& operator=(FileFooterSection&& other) = default;
	};
}

class SSTable
{
public:
	SSTable() = default;
	explicit SSTable(const std::filesystem::path& path);

	SSTable(const SSTable&) = delete;
	SSTable& operator=(const SSTable&) = delete;

	SSTable(SSTable&&) noexcept = default;
	SSTable& operator=(SSTable&&) noexcept = default;
	//SSTable(const MemTable& mem_table, std::uint32_t id);

private:

	const std::filesystem::path path;

	std::unique_ptr<ReadableFile> file_in;
	std::unique_ptr<WritableFile> file_out;
	SSTableEntities::FileHeaderSection file_header_section{};
	SSTableEntities::DataSection data_section{};
	SSTableEntities::DataSectionView data_section_view{};
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

	static std::optional<SSTable> load(const std::filesystem::path& path, Arena& arena);
};

class SSTableManager
{
public:
	SSTableManager() = default;

private:
	SSTableWriter sstable_writer;
	SSTableLoader sstable_loader;

	std::vector<SSTable> immutable_pool;
	std::vector<SSTable> pool;

	static std::filesystem::path make_table_path(const std::filesystem::path& dir, std::uint32_t table_id);
	static std::filesystem::path make_tmp_table_path(const std::filesystem::path& dir, std::uint32_t table_id);

public:

	void write_latest(bool erase = false);
	void write_all(bool erase = false);
	void add_to_pool(SSTable&& sstable);
	//void add_to_pool(MemTable& mem_table);
	void load(Arena& arena, const std::filesystem::path& root_path);
	//void get_latest();
};
