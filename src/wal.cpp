#include "wal.h"

Wal::Wal(const std::string& path)
	: read_file(path, std::ios::binary),
	  write_file(path, std::ios::binary | std::ios::app),
	  bytes_read(0)
{
	if (!read_file) throw std::runtime_error("Failed to open wal to read");
	if (!write_file) throw std::runtime_error("Failed to open wal to write");
	bytes_written = static_cast<int>(write_file.tellp()) % Wal::BLOCK_SIZE;
}

int Wal::Header::get_size()
{
	return (
		sizeof(uint32_t)+
		sizeof(uint64_t) +
		sizeof(uint64_t) +
		sizeof(Type) +
		sizeof(uint64_t)
	);
}
int Wal::Header::get_size() const
{
	return (
		sizeof(uint32_t) +
		sizeof(uint64_t) +
		sizeof(uint64_t) +
		sizeof(Type) +
		sizeof(uint64_t)
		);
}

int Wal::Payload::get_size()
{
	return (
		sizeof(uint32_t) +
		sizeof(uint32_t) +
		key.size() +
		value.size()
	);
}
int Wal::Payload::get_size() const 
{
	return (
		sizeof(uint32_t) +
		sizeof(uint32_t) +
		key.size() +
		value.size()
		);
}

int Wal::Entry::get_size()
{
	return header.get_size() + payload.get_size();
}
int Wal::Entry::get_size() const 
{
	return header.get_size() + payload.get_size();
}

Wal::Header::Header(const Payload& payload, Type type, uint64_t seq_num)
	: type(type),
	seq_num(seq_num),
	magic(Wal::MAGIC),
	payload_size(static_cast<uint64_t>(payload.get_size())),
	crc(Wal::compute_hash(payload))
{
}

Wal::Entry::Entry(const ByteRecord& record, uint64_t seq_num)
	: payload(record)
{
	header = std::move(Wal::Header(payload, record.type, seq_num));
}
Wal::Entry::Entry()
	: header(), payload()
{
}
Wal::Entry::Entry(const Bytes& key, const Bytes& value, Type type, uint64_t seq_num)
	: payload(key, value)
{
	header = std::move(Wal::Header(payload, type, seq_num));
}

void Wal::Entry::check() const
{
	if (header.magic != Wal::MAGIC)
		throw std::runtime_error("Corrupted (WAL): magic doesn't correspond");
	if (header.crc != Wal::compute_hash(payload))
		throw std::runtime_error("Corrupted (WAL): crc32 doesn't correspond");
	if (header.payload_size != payload.get_size())
		throw std::runtime_error("Corrupted (WAL): payload size doesn't correspond");
	if (payload.key_size != payload.key.size())
		throw std::runtime_error("Corrupted (WAL): key size doesn't correspond");
	if (payload.value_size != payload.value.size())
		throw std::runtime_error("Corrupted (WAL): value size doesn't correspond");
}

Wal::Payload::Payload(const ByteRecord& record)
	: key_size(record.key.size()), value_size(record.value.size()), key(record.key), value(record.value)
{
}
Wal::Payload::Payload(const Bytes& key, const Bytes& value)
	: key_size(key.size()), value_size(value.size()), key(key), value(value)
{
}

uint64_t Wal::compute_hash(const Payload& record)
{
	uint64_t h = 5381;

	auto hash_bytes = [&](const uint8_t* data, size_t size)
		{
			for (size_t i = 0; i < size; ++i)
				h = ((h << 5) + h) + data[i]; // h * 33 + byte
		};

	// hash key_size
	hash_bytes(reinterpret_cast<const uint8_t*>(&record.key_size), sizeof(record.key_size));

	// hash value_size
	hash_bytes(reinterpret_cast<const uint8_t*>(&record.value_size), sizeof(record.value_size));

	// hash key
	hash_bytes(reinterpret_cast<const uint8_t*>(record.key.data()), record.key_size);

	// hash value
	hash_bytes(reinterpret_cast<const uint8_t*>(record.value.data()), record.value_size);

	return h;
}

Wal::Status Wal::write(const Wal::Entry& entry)
{
	if (entry.get_size() > BLOCK_SIZE)
		return Wal::Status::EntryTooLarge;

	if (bytes_written + entry.get_size() > BLOCK_SIZE)
		next_block(bytes_written, write_file);

	Wal::write_header(entry.header);
	Wal::write_payload(entry.payload);
	
	return Wal::Status::OK;
}

void Wal::read_raw(char* ptr, std::streamsize size)
{
	read_file.read(ptr, size);
	bytes_read += static_cast<int>(size);
	if (!read_file)
		throw std::runtime_error("Failed to read WAL");
}

std::variant<Wal::Status, InternalRecord> Wal::read_next()
{
	if (read_file.peek() == EOF)
		return Wal::Status::EndOfFile;

	if (read_file.peek() == 0)
		next_block(bytes_read, read_file);

	if (read_file.peek() == EOF)
		return Wal::Status::EndOfFile;

	Entry entry;
	read_header(entry.header);
	read_payload(entry.payload);
	
	entry.check();
	
	return InternalRecord(entry.payload.key, entry.payload.value, entry.header.type, entry.header.seq_num);
}

void Wal::write_header(const Wal::Header& header)
{
	Wal::write_bytes(header.magic);
	Wal::write_bytes(header.crc);
	Wal::write_bytes(header.seq_num);
	Wal::write_bytes(header.type);
	Wal::write_bytes(header.payload_size);
}
void Wal::write_payload(const Wal::Payload& payload)
{
	Wal::write_bytes(payload.key_size);
	Wal::write_bytes(payload.key);
	Wal::write_bytes(payload.value_size);
	Wal::write_bytes(payload.value);
}

void Wal::read_header(Wal::Header& header)
{
	read_bytes(header.magic);
	read_bytes(header.crc);
	read_bytes(header.seq_num);
	read_bytes(header.type);
	read_bytes(header.payload_size);
}
void Wal::read_payload(Wal::Payload& payload)
{
	read_bytes(payload.key_size);
	payload.key.resize(payload.key_size);
	char* ptr = reinterpret_cast<char*>(payload.key.data());
	read_raw(ptr, payload.key_size);

	read_bytes(payload.value_size);
	payload.value.resize(payload.value_size);
	ptr = reinterpret_cast<char*>(payload.value.data());
	read_raw(ptr, payload.value_size);
}