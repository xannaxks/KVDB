#include "mem_table.h"
#include <fstream>
#include <type_traits>
#include "bytes.h"
#include "sstable.h"
#include <cstdint>

//class Wal
//{
//public:
//	static const uint32_t MAGIC = 0xDEADBEEF;
//	static const int BLOCK_SIZE = 4096;
//	static enum class Status
//	{
//		OK = 1,
//		EntryTooLarge = 2,
//		EndOfFile = 3
//	};
//
//private:
//	std::ifstream read_file;
//	std::ofstream write_file;
//	int bytes_written, bytes_read;
//
//	template<typename T>
//	static void next_block(int& bytes_processed, T& file)
//	{
//		int remaining = BLOCK_SIZE - bytes_processed;
//		static const char zeros[4096] = {};
//
//		if constexpr (std::is_same_v<std::remove_cvref_t<T>, std::ifstream>)
//			file.seekg(remaining, std::ios::cur);
//		else
//			file.write(zeros, remaining);
//
//		bytes_processed = 0;
//	}
//
//	struct Payload
//	{
//		uint32_t key_size;
//		uint32_t value_size;
//		Bytes key, value;
//
//		int get_size() const;
//		int get_size();
//
//		Payload() = default;
//		Payload(const ByteRecord& record);
//		Payload(const Bytes& key, const Bytes& value);
//	};
//	static uint64_t compute_hash(const Payload& record);
//	struct Header
//	{
//		uint32_t magic;
//		uint64_t crc;
//		uint64_t seq_num;
//		Type type;
//		uint64_t payload_size;
//
//		int get_size();
//		int get_size() const;
//
//		Header() = default;
//		Header(const Payload& payload, Type type, uint64_t seq_num);
//	};
//
//	template<typename T>
//	void write_bytes(const T& obj)
//	{
//		const char* ptr = nullptr;
//		std::streamsize size = 0;
//
//		if constexpr (std::is_same_v<std::remove_cvref_t<T>, Bytes>)
//		{
//			ptr = reinterpret_cast<const char*>(obj.data());
//			size = static_cast<std::streamsize>(obj.size());
//		}
//		else
//		{
//			ptr = reinterpret_cast<const char*>(&obj);
//			size = static_cast<std::streamsize>(sizeof(obj));
//		}
//
//		write_file.write(ptr, size);
//		bytes_written += size;
//
//		if (!write_file)
//			throw std::runtime_error("Failed to write WAL");
//	}
//		
//	template<typename T>
//	void read_bytes(T& buffer)
//	{
//		read_file.read(reinterpret_cast<char*>(&buffer), sizeof(buffer));
//		bytes_read += sizeof(buffer);
//		if (!read_file)
//			throw std::runtime_error("Failed to read WAL");
//	}
//	void read_raw(char* ptr, std::streamsize size);
//
//	void write_header(const Header& header);
//	void write_payload(const Payload& payload);
//
//	void read_header( Header& header);
//	void read_payload(Payload& payload);
//
//public:
//	Wal(const std::string& path);
//
//	struct Entry
//	{
//		Wal::Header header;
//		Wal::Payload payload;
//
//		Entry(const ByteRecord& record, uint64_t seq_num);
//		Entry(const Bytes& key, const Bytes& value, Type type, uint64_t seq_num);
//		Entry();
//
//		int get_size();
//		int get_size() const;
//
//		void check() const;
//	};
//
//	std::variant<Wal::Status, InternalRecord> read_next();
//	Wal::Status write(const Entry& record);
//};

class WAL;
class WALWriter;
class WALLoader;
class WALManager;

struct WALFileHeader
{
	uint32_t magic;
	uint32_t version;
	uint32_t header_size;

	uint64_t wal_id;
	uint64_t start_seq;

	uint32_t block_size;
	uint32_t reserved;

	uint32_t header_crc32;

	void write(std::ofstream& file);
	static std::optional<WALFileHeader> load(std::ifstream& file, uint32_t wal_id);
	bool self_check();

	static uint32_t disk_size();

	WALFileHeader() = default;
	WALFileHeader(uint32_t wal_id, uint64_t seq);
};

struct Fragment
{
	enum class Type : uint8_t
	{
		FULL = 0,
		FIRST = 1,
		MIDDLE = 2,
		LAST = 3,
	};
	struct Header
	{
		uint32_t fragment_crc32;
		uint32_t header_size;

		Fragment::Type fragment_type;
		::Type type;

		uint64_t seq_num;

		uint32_t fragment_size;

		static std::size_t disk_size();

		void write(std::ofstream& file);
		static std::optional<Header> load(std::ifstream& file);
	};
	struct Payload
	{
		std::vector<std::byte> bytes;

		void write(std::ofstream& file);
		static std::optional<Payload> load(std::ifstream& file);

		std::size_t disk_size();
	};
	Header header;
	Payload payload;

	void compute_crc32();
	static void compute_crc32(uint32_t& crc32, Fragment& fragment);

	void write(std::ofstream& file);
	static std::optional<Fragment> load(std::ifstream& file);

	std::size_t disk_size();
};

class WALWriter
{
private:
	std::ofstream& file;
	std::string current_wal_file_name;
	uint32_t wal_id;

public:

	void write(InternalRecord& internal_record, bool new_file);
	void write_payload(std::vector<std::byte>& payload_sequence, InternalRecord& type);
};
class WALLoader
{
public:

	struct LoadResult
	{
		std::vector<InternalRecord> records;
		uint64_t last_valid_offset;
		bool ok;
		bool had_torn_tail, had_corruption;
		std::string error;
	};

	static LoadResult load(std::ifstream& file, std::string& current_wal_file_name, uint32_t wal_id, Arena& arena);
	static auto build(std::vector<InternalRecord>& internla_record);

	friend class WAL;
};

class WAL
{
public:

	WALWriter wal_writer;
	WALLoader wal_loader;

	void write(InternalRecord& internal_record, bool new_file = false);
	void load();
};
