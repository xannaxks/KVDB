#include "mem_table.h"
#include <fstream>
#include <type_traits>
#include "bytes.h"
#include <cstdint>

class Wal
{
public:
	static const uint32_t MAGIC = 0xDEADBEEF;
	static const int BLOCK_SIZE = 4096;
	static enum class Status
	{
		OK = 1,
		EntryTooLarge = 2,
		EndOfFile = 3
	};

private:
	std::ifstream read_file;
	std::ofstream write_file;
	int bytes_written, bytes_read;

	template<typename T>
	static void next_block(int& bytes_processed, T& file)
	{
		int remaining = BLOCK_SIZE - bytes_processed;
		static const char zeros[4096] = {};

		if constexpr (std::is_same_v<std::remove_cvref_t<T>, std::ifstream>)
			file.seekg(remaining, std::ios::cur);
		else
			file.write(zeros, remaining);

		bytes_processed = 0;
	}

	struct Payload
	{
		uint32_t key_size;
		uint32_t value_size;
		Bytes key, value;

		int get_size() const;
		int get_size();

		Payload() = default;
		Payload(const ByteRecord& record);
		Payload(const Bytes& key, const Bytes& value);
	};
	static uint64_t compute_hash(const Payload& record);
	struct Header
	{
		uint32_t magic;
		uint64_t crc;
		uint64_t seq_num;
		Type type;
		uint64_t payload_size;

		int get_size();
		int get_size() const;

		Header() = default;
		Header(const Payload& payload, Type type, uint64_t seq_num);
	};

	template<typename T>
	void write_bytes(const T& obj)
	{
		const char* ptr = nullptr;
		std::streamsize size = 0;

		if constexpr (std::is_same_v<std::remove_cvref_t<T>, Bytes>)
		{
			ptr = reinterpret_cast<const char*>(obj.data());
			size = static_cast<std::streamsize>(obj.size());
		}
		else
		{
			ptr = reinterpret_cast<const char*>(&obj);
			size = static_cast<std::streamsize>(sizeof(obj));
		}

		write_file.write(ptr, size);
		bytes_written += size;

		if (!write_file)
			throw std::runtime_error("Failed to write WAL");
	}
		
	template<typename T>
	void read_bytes(T& buffer)
	{
		read_file.read(reinterpret_cast<char*>(&buffer), sizeof(buffer));
		bytes_read += sizeof(buffer);
		if (!read_file)
			throw std::runtime_error("Failed to read WAL");
	}
	void read_raw(char* ptr, std::streamsize size);

	void write_header(const Header& header);
	void write_payload(const Payload& payload);

	void read_header( Header& header);
	void read_payload(Payload& payload);

public:
	Wal(const std::string& path);

	struct Entry
	{
		Wal::Header header;
		Wal::Payload payload;

		Entry(const ByteRecord& record, uint64_t seq_num);
		Entry(const Bytes& key, const Bytes& value, Type type, uint64_t seq_num);
		Entry();

		int get_size();
		int get_size() const;

		void check() const;
	};

	std::variant<Wal::Status, InternalRecord> read_next();
	Wal::Status write(const Entry& record);
};