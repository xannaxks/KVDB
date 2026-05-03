#include "wal.h"

//Wal::Wal(const std::string& path)
//	: read_file(path, std::ios::binary),
//	  write_file(path, std::ios::binary | std::ios::app),
//	  bytes_read(0)
//{
//	if (!read_file) throw std::runtime_error("Failed to open wal to read");
//	if (!write_file) throw std::runtime_error("Failed to open wal to write");
//	bytes_written = static_cast<int>(write_file.tellp()) % Wal::BLOCK_SIZE;
//}
//
//int Wal::Header::get_size()
//{
//	return (
//		sizeof(uint32_t)+
//		sizeof(uint64_t) +
//		sizeof(uint64_t) +
//		sizeof(Type) +
//		sizeof(uint64_t)
//	);
//}
//int Wal::Header::get_size() const
//{
//	return (
//		sizeof(uint32_t) +
//		sizeof(uint64_t) +
//		sizeof(uint64_t) +
//		sizeof(Type) +
//		sizeof(uint64_t)
//		);
//}
//
//int Wal::Payload::get_size()
//{
//	return (
//		sizeof(uint32_t) +
//		sizeof(uint32_t) +
//		key.size() +
//		value.size()
//	);
//}
//int Wal::Payload::get_size() const 
//{
//	return (
//		sizeof(uint32_t) +
//		sizeof(uint32_t) +
//		key.size() +
//		value.size()
//		);
//}
//
//int Wal::Entry::get_size()
//{
//	return header.get_size() + payload.get_size();
//}
//int Wal::Entry::get_size() const 
//{
//	return header.get_size() + payload.get_size();
//}
//
//Wal::Header::Header(const Payload& payload, Type type, uint64_t seq_num)
//	: type(type),
//	seq_num(seq_num),
//	magic(Wal::MAGIC),
//	payload_size(static_cast<uint64_t>(payload.get_size())),
//	crc(Wal::compute_hash(payload))
//{
//}
//
//Wal::Entry::Entry(const ByteRecord& record, uint64_t seq_num)
//	: payload(record)
//{
//	header = std::move(Wal::Header(payload, record.type, seq_num));
//}
//Wal::Entry::Entry()
//	: header(), payload()
//{
//}
//Wal::Entry::Entry(const Bytes& key, const Bytes& value, Type type, uint64_t seq_num)
//	: payload(key, value)
//{
//	header = std::move(Wal::Header(payload, type, seq_num));
//}
//
//void Wal::Entry::check() const
//{
//	if (header.magic != Wal::MAGIC)
//		throw std::runtime_error("Corrupted (WAL): magic doesn't correspond");
//	if (header.crc != Wal::compute_hash(payload))
//		throw std::runtime_error("Corrupted (WAL): crc32 doesn't correspond");
//	if (header.payload_size != payload.get_size())
//		throw std::runtime_error("Corrupted (WAL): payload size doesn't correspond");
//	if (payload.key_size != payload.key.size())
//		throw std::runtime_error("Corrupted (WAL): key size doesn't correspond");
//	if (payload.value_size != payload.value.size())
//		throw std::runtime_error("Corrupted (WAL): value size doesn't correspond");
//}
//
//Wal::Payload::Payload(const ByteRecord& record)
//	: key_size(record.key.size()), value_size(record.value.size()), key(record.key), value(record.value)
//{
//}
//Wal::Payload::Payload(const Bytes& key, const Bytes& value)
//	: key_size(key.size()), value_size(value.size()), key(key), value(value)
//{
//}
//
//uint64_t Wal::compute_hash(const Payload& record)
//{
//	uint64_t h = 5381;
//
//	auto hash_bytes = [&](const uint8_t* data, size_t size)
//		{
//			for (size_t i = 0; i < size; ++i)
//				h = ((h << 5) + h) + data[i]; // h * 33 + byte
//		};
//
//	// hash key_size
//	hash_bytes(reinterpret_cast<const uint8_t*>(&record.key_size), sizeof(record.key_size));
//
//	// hash value_size
//	hash_bytes(reinterpret_cast<const uint8_t*>(&record.value_size), sizeof(record.value_size));
//
//	// hash key
//	hash_bytes(reinterpret_cast<const uint8_t*>(record.key.data()), record.key_size);
//
//	// hash value
//	hash_bytes(reinterpret_cast<const uint8_t*>(record.value.data()), record.value_size);
//
//	return h;
//}
//
//Wal::Status Wal::write(const Wal::Entry& entry)
//{
//	if (entry.get_size() > BLOCK_SIZE)
//		return Wal::Status::EntryTooLarge;
//
//	if (bytes_written + entry.get_size() > BLOCK_SIZE)
//		next_block(bytes_written, write_file);
//
//	Wal::write_header(entry.header);
//	Wal::write_payload(entry.payload);
//	
//	return Wal::Status::OK;
//}
//
//void Wal::read_raw(char* ptr, std::streamsize size)
//{
//	read_file.read(ptr, size);
//	bytes_read += static_cast<int>(size);
//	if (!read_file)
//		throw std::runtime_error("Failed to read WAL");
//}
//
//std::variant<Wal::Status, InternalRecord> Wal::read_next()
//{
//	if (read_file.peek() == EOF)
//		return Wal::Status::EndOfFile;
//
//	if (read_file.peek() == 0)
//		next_block(bytes_read, read_file);
//
//	if (read_file.peek() == EOF)
//		return Wal::Status::EndOfFile;
//
//	Entry entry;
//	read_header(entry.header);
//	read_payload(entry.payload);
//	
//	entry.check();
//	
//	return InternalRecord(entry.payload.key, entry.payload.value, entry.header.type, entry.header.seq_num);
//}
//
//void Wal::write_header(const Wal::Header& header)
//{
//	Wal::write_bytes(header.magic);
//	Wal::write_bytes(header.crc);
//	Wal::write_bytes(header.seq_num);
//	Wal::write_bytes(header.type);
//	Wal::write_bytes(header.payload_size);
//}
//void Wal::write_payload(const Wal::Payload& payload)
//{
//	Wal::write_bytes(payload.key_size);
//	Wal::write_bytes(payload.key);
//	Wal::write_bytes(payload.value_size);
//	Wal::write_bytes(payload.value);
//}
//
//void Wal::read_header(Wal::Header& header)
//{
//	read_bytes(header.magic);
//	read_bytes(header.crc);
//	read_bytes(header.seq_num);
//	read_bytes(header.type);
//	read_bytes(header.payload_size);
//}
//void Wal::read_payload(Wal::Payload& payload)
//{
//	read_bytes(payload.key_size);
//	payload.key.resize(payload.key_size);
//	char* ptr = reinterpret_cast<char*>(payload.key.data());
//	read_raw(ptr, payload.key_size);
//
//	read_bytes(payload.value_size);
//	payload.value.resize(payload.value_size);
//	ptr = reinterpret_cast<char*>(payload.value.data());
//	read_raw(ptr, payload.value_size);
//}


// disk size 

std::size_t Fragment::Header::disk_size()
{
	return sizeof(fragment_crc32) +
		sizeof(header_size) +
		sizeof(Fragment::Type) +
		sizeof(::Type) +
		sizeof(seq_num) +
		sizeof(fragment_size);
}
std::size_t Fragment::Payload::disk_size()
{
	return bytes.size();
}
std::size_t Fragment::disk_size()
{
	return Fragment::Header::disk_size() + this->payload.disk_size();
}
uint32_t WALFileHeader::disk_size()
{
	return (
		sizeof(magic) +
		sizeof(version) +
		sizeof(header_size) +
		sizeof(wal_id) +
		sizeof(start_seq) +
		sizeof(block_size) +
		sizeof(reserved) +
		sizeof(header_crc32)
		);
}


// crc computing


void Fragment::compute_crc32()
{
	this->header.fragment_crc32 = ::crc32(0L, Z_NULL, 0);
	crc32_add_pod(this->header.fragment_crc32, this->header.header_size);
	crc32_add_pod(this->header.fragment_crc32, this->header.fragment_type);
	crc32_add_pod(this->header.fragment_crc32, this->header.type);
	crc32_add_pod(this->header.fragment_crc32, this->header.seq_num);
	crc32_add_pod(this->header.fragment_crc32, this->header.fragment_size);

	::compute_crc32(this->header.fragment_crc32, this->payload.bytes.data(), this->payload.disk_size());
}
void Fragment::compute_crc32(uint32_t& crc32, Fragment& fragment)
{
	crc32 = ::crc32(0L, Z_NULL, 0);
	crc32_add_pod(crc32, fragment.header.header_size);
	crc32_add_pod(crc32, fragment.header.fragment_type);
	crc32_add_pod(crc32, fragment.header.type);
	crc32_add_pod(crc32, fragment.header.seq_num);
	crc32_add_pod(crc32, fragment.header.fragment_size);

	::compute_crc32(crc32, fragment.payload.bytes.data(), fragment.payload.disk_size());
}


/// Constructors 


WALFileHeader::WALFileHeader(uint32_t wal_id, uint64_t seq)
	: magic(WAL_FILE_MAGIC),
	version(WAL_VERSION),
	header_size(WALFileHeader::disk_size()),
	wal_id(wal_id),
	start_seq(seq),
	block_size(WAL_FILE_BLOCK_SIZE),
	reserved(0)
{
	this->header_crc32 = ::crc32(0L, Z_NULL, 0);

	crc32_add_pod(this->header_crc32,this->magic);
	crc32_add_pod(this->header_crc32, this->version);
	crc32_add_pod(this->header_crc32, this->header_size);

	crc32_add_pod(this->header_crc32, this->wal_id);
	crc32_add_pod(this->header_crc32, this->start_seq);

	crc32_add_pod(this->header_crc32, this->block_size);
	crc32_add_pod(this->header_crc32, this->reserved);
}


// Writers

void Fragment::Header::write(std::ofstream& file)
{
	file.write(reinterpret_cast<const char*>(&this->fragment_crc32), sizeof(this->fragment_crc32));
	file.write(reinterpret_cast<const char*>(&this->header_size), sizeof(this->header_size));
	file.write(reinterpret_cast<const char*>(&this->fragment_type), sizeof(this->fragment_type));
	file.write(reinterpret_cast<const char*>(&this->type), sizeof(this->type));
	file.write(reinterpret_cast<const char*>(&this->seq_num), sizeof(this->seq_num));
	file.write(reinterpret_cast<const char*>(&this->fragment_size), sizeof(this->fragment_size));
}

void Fragment::Payload::write(std::ofstream& file)
{
	file.write(reinterpret_cast<const char*>(this->bytes.data()), this->bytes.size());
}

void Fragment::write(std::ofstream& file)
{
	this->header.write(file);
	this->payload.write(file);
}

void WALFileHeader::write(std::ofstream& file)
{
	uint64_t offset = file.tellp();

	ensure_fits_in_block(offset, WALFileHeader::disk_size(), WAL_FILE_BLOCK_SIZE, file);
	write_to_stream(file, offset, reinterpret_cast<void*>(&this->magic), sizeof(this->magic), WAL_FILE_BLOCK_SIZE);
	write_to_stream(file, offset, reinterpret_cast<void*>(&this->version), sizeof(this->version), WAL_FILE_BLOCK_SIZE);
	write_to_stream(file, offset, reinterpret_cast<void*>(&this->header_size), sizeof(this->header_size), WAL_FILE_BLOCK_SIZE);

	write_to_stream(file, offset, reinterpret_cast<void*>(&this->wal_id), sizeof(this->wal_id), WAL_FILE_BLOCK_SIZE);
	write_to_stream(file, offset, reinterpret_cast<void*>(&this->start_seq), sizeof(this->start_seq), WAL_FILE_BLOCK_SIZE);

	write_to_stream(file, offset, reinterpret_cast<void*>(&this->block_size), sizeof(this->block_size), WAL_FILE_BLOCK_SIZE);
	write_to_stream(file, offset, reinterpret_cast<void*>(&this->reserved), sizeof(this->reserved), WAL_FILE_BLOCK_SIZE);

	write_to_stream(file, offset, reinterpret_cast<void*>(&this->header_crc32), sizeof(this->header_crc32), WAL_FILE_BLOCK_SIZE);
}

void WALWriter::write(InternalRecord& internal_record, bool new_file)
{
	if (new_file)
	{
		this->file.close();
		this->current_wal_file_name = get_next_wal_file_name();
		this->file.open(this->current_wal_file_name, std::ios::binary | std::ios::trunc);

		WALFileHeader wal_file_header(this->wal_id, internal_record.seq_num);
		wal_file_header.write(file);
	}

	std::vector<std::byte> payload_sequence = std::move(internal_record.return_byte_sequence());
	WALWriter::write_payload(payload_sequence, internal_record);
}

void WALWriter::write_payload(std::vector<std::byte>& payload_sequence, InternalRecord& internal_record)
{
	std::size_t payload_sequence_pos = 0;
	bool first = true;
	uint64_t offset = static_cast<uint64_t>(file.tellp());

	while (payload_sequence.size() > payload_sequence_pos)
	{
		::ensure_fits_in_block(offset, Fragment::Header::disk_size(), BLOCK_SIZE, file);
		uint32_t remaining_in_block = BLOCK_SIZE - (offset % BLOCK_SIZE);

		uint32_t max_payload_size_in_fragment = remaining_in_block - Fragment::Header::disk_size();
		uint32_t payload_sequence_left = payload_sequence.size() - payload_sequence_pos;
		uint32_t fragment_payload_size = std::min(max_payload_size_in_fragment, payload_sequence_left);

		Fragment::Type fragment_type;

		bool last = (fragment_payload_size == payload_sequence_left);

		if (first && last)
			fragment_type = Fragment::Type::FULL;
		else if (first)
			fragment_type = Fragment::Type::FIRST;
		else if (last)
			fragment_type = Fragment::Type::LAST;
		else
			fragment_type = Fragment::Type::MIDDLE;

		Fragment fragment;
		fragment.header.header_size = Fragment::Header::disk_size();
		fragment.header.fragment_type = fragment_type;
		fragment.header.type = internal_record.type;
		fragment.header.seq_num = internal_record.seq_num;
		fragment.header.fragment_size = fragment_payload_size;

		fragment.payload.bytes.insert(
			fragment.payload.bytes.end(),
			payload_sequence.begin() + payload_sequence_pos,
			payload_sequence.begin() + payload_sequence_pos + fragment_payload_size
		);

		fragment.compute_crc32();

		fragment.write(file);
		
		offset = static_cast<uint64_t>(file.tellp());
		payload_sequence_pos += fragment_payload_size;
		first = false;
	}
}

void WAL::write(InternalRecord& internal_record, bool new_file)
{
	this->wal_writer.write(internal_record, new_file);
}


// Loaders

std::optional<WALFileHeader> WALFileHeader::load(std::ifstream& file, uint32_t must_be_wal_id)
{
	WALFileHeader wal_file_header{};
	uint64_t offset = file.tellg();
	
	ensure_fits_in_block(file, WALFileHeader::disk_size(), WAL_FILE_BLOCK_SIZE);

	file.read(reinterpret_cast<char*>(&wal_file_header.magic), static_cast<std::streamsize>(sizeof(wal_file_header.magic)));
	if (!file) return std::nullopt;
	if (wal_file_header.magic != WAL_FILE_MAGIC) return std::nullopt;

	file.read(reinterpret_cast<char*>(&wal_file_header.version), static_cast<std::streamsize>(sizeof(wal_file_header.version)));
	if (!file) return std::nullopt;
	if (wal_file_header.version != WAL_VERSION) return std::nullopt;

	file.read(reinterpret_cast<char*>(&wal_file_header.header_size), static_cast<std::streamsize>(sizeof(wal_file_header.header_size)));
	if (!file) return std::nullopt;
	if (wal_file_header.header_size != WALFileHeader::disk_size()) return std::nullopt;

	file.read(reinterpret_cast<char*>(&wal_file_header.wal_id), static_cast<std::streamsize>(sizeof(wal_file_header.wal_id)));
	if (!file) return std::nullopt;
	if (wal_file_header.wal_id != must_be_wal_id) return std::nullopt;

	file.read(reinterpret_cast<char*>(&wal_file_header.start_seq), static_cast<std::streamsize>(sizeof(wal_file_header.start_seq)));
	if (!file) return std::nullopt;

	file.read(reinterpret_cast<char*>(&wal_file_header.block_size), static_cast<std::streamsize>(sizeof(wal_file_header.block_size)));
	if (!file) return std::nullopt;
	if (wal_file_header.block_size != WAL_FILE_BLOCK_SIZE) return std::nullopt;

	file.read(reinterpret_cast<char*>(&wal_file_header.reserved), static_cast<std::streamsize>(sizeof(wal_file_header.reserved)));
	if (!file) return std::nullopt;

	file.read(reinterpret_cast<char*>(&wal_file_header.header_crc32), static_cast<std::streamsize>(sizeof(wal_file_header.header_crc32)));
	if (!file) return std::nullopt;

	uint32_t must_be_header_crc32;
	must_be_header_crc32 = ::crc32(0L, Z_NULL, 0);
	crc32_add_pod(must_be_header_crc32, wal_file_header.magic);
	crc32_add_pod(must_be_header_crc32, wal_file_header.version);
	crc32_add_pod(must_be_header_crc32, wal_file_header.header_size);
	crc32_add_pod(must_be_header_crc32, wal_file_header.wal_id);
	crc32_add_pod(must_be_header_crc32, wal_file_header.start_seq);
	crc32_add_pod(must_be_header_crc32, wal_file_header.block_size);
	crc32_add_pod(must_be_header_crc32, wal_file_header.reserved);

	if (must_be_header_crc32 != wal_file_header.header_crc32) return std::nullopt;

	return wal_file_header;
}
std::optional<Fragment> Fragment::load(std::ifstream& file)
{
	Fragment fragment;
	uint64_t offset = file.tellg();

	ensure_fits_in_block(file, Fragment::Header::disk_size(), WAL_FILE_BLOCK_SIZE);

	file.read(reinterpret_cast<char*>(&fragment.header.fragment_crc32), static_cast<std::streamsize>(sizeof(fragment.header.fragment_crc32)));
	if (!file) return std::nullopt;

	file.read(reinterpret_cast<char*>(&fragment.header.header_size), static_cast<std::streamsize>(sizeof(fragment.header.header_size)));
	if (!file) return std::nullopt;
	if (fragment.header.header_size != Fragment::Header::disk_size()) return std::nullopt;

	file.read(reinterpret_cast<char*>(&fragment.header.fragment_type), static_cast<std::streamsize>(sizeof(fragment.header.fragment_type)));
	if (!file) return std::nullopt;
	
	file.read(reinterpret_cast<char*>(&fragment.header.type), static_cast<std::streamsize>(sizeof(fragment.header.type)));
	if (!file) return std::nullopt;

	file.read(reinterpret_cast<char*>(&fragment.header.seq_num), static_cast<std::streamsize>(sizeof(fragment.header.seq_num)));
	if (!file) return std::nullopt;

	file.read(reinterpret_cast<char*>(&fragment.header.fragment_size), static_cast<std::streamsize>(sizeof(fragment.header.fragment_size)));
	if (!file) return std::nullopt;

	fragment.payload.bytes.resize(fragment.header.fragment_size);
	file.read(reinterpret_cast<char*>(fragment.payload.bytes.data()), static_cast<std::streamsize>(fragment.header.fragment_size));
	if (!file) return std::nullopt;

	uint32_t must_be_crc32;
	Fragment::compute_crc32(must_be_crc32, fragment);

	if (must_be_crc32 != fragment.header.fragment_crc32) return std::nullopt;

	return fragment;
}

bool restore_logical_record(InternalRecord& internal_record, std::vector<std::byte>& payload_buffer, Fragment& fragment, Arena& arena)
{
	static bool rebuilding = false;
	static uint64_t current_seq = 0;
	static ::Type current_type = ::Type::Undefined;

	if (fragment.header.fragment_type == Fragment::Type::FULL || fragment.header.fragment_type == Fragment::Type::FIRST)
	{
		if (!payload_buffer.empty()) return false;
		internal_record.seq_num = fragment.header.seq_num;
		internal_record.type = fragment.header.type;

		rebuilding = true;
		current_seq = fragment.header.seq_num;
		current_type = fragment.header.type;
	}

	if (rebuilding == false) return false;
	if (current_seq != fragment.header.seq_num) return false;
	if (current_type != fragment.header.type) return false;
	
	for (std::byte& i : fragment.payload.bytes)
		payload_buffer.emplace_back(i);
	
	if (fragment.header.fragment_type != Fragment::Type::LAST && fragment.header.fragment_type != Fragment::Type::FULL)
		return true;

	if (payload_buffer.size() < sizeof(ArenaEntry::size) * 2) return false;

	std::size_t fragment_payload_pos = 0;

	std::byte* size_ptr = reinterpret_cast<std::byte*>(&internal_record.key_entry.size);
	for (std::size_t i = 0; i < sizeof(ArenaEntry::size); i++)
	{
		*size_ptr = payload_buffer[fragment_payload_pos++];
	}
	size_ptr = reinterpret_cast<std::byte*>(&internal_record.value_entry.size);
	for (std::size_t i = 0; i < sizeof(ArenaEntry::size); i++)
	{
		*size_ptr = payload_buffer[fragment_payload_pos++];
	}

	if (payload_buffer.size() != sizeof(ArenaEntry::size) * 2
		+ internal_record.key_entry.size
		+ internal_record.value_entry.size
	)
		return false;

	internal_record.key_entry.data = arena.alloc(internal_record.key_entry.size, alignof(std::byte));
	internal_record.value_entry.data = arena.alloc(internal_record.value_entry.size, alignof(std::byte));

	for (std::size_t i = 0; i < internal_record.key_entry.size; i++)
	{
		*(reinterpret_cast<std::byte*>(internal_record.key_entry.data) + i) = payload_buffer[fragment_payload_pos++];
	}
	for (std::size_t i = 0; i < internal_record.value_entry.size; i++)
	{
		*(reinterpret_cast<std::byte*>(internal_record.value_entry.data) + i) = payload_buffer[fragment_payload_pos++];
	}

	rebuilding = false;
	current_seq = 0;
	current_type = ::Type::Undefined;


	return true;
}

WALLoader::LoadResult WALLoader::load(
	std::ifstream& file,
	std::string& current_wal_file,
	uint32_t wal_id,
	Arena& arena
) {
	LoadResult result;

	auto header = WALFileHeader::load(file, wal_id);
	if (!header) {
		result.ok = false;
		result.had_corruption = true;
		result.error = "Invalid WAL file header";
		return result;
	}

	result.last_valid_offset = static_cast<uint64_t>(file.tellg());

	InternalRecord logical_record{};
	std::vector<std::byte> payload_buffer;

	while (true) {
		uint64_t fragment_start_offset = static_cast<uint64_t>(file.tellg());

		std::optional<Fragment> fragment = Fragment::load(file);

		if (!fragment) {
			if (!payload_buffer.empty()) {
				result.had_torn_tail = true;
			}

			break;
		}

		if (!restore_logical_record(logical_record, payload_buffer, *fragment, arena)) {
			result.had_corruption = true;
			result.ok = false;
			result.error = "Invalid WAL fragment sequence";
			break;
		}

		if (fragment->header.fragment_type != Fragment::Type::LAST &&
			fragment->header.fragment_type != Fragment::Type::FULL) {
			continue;
		}

		result.records.emplace_back(logical_record);
		result.last_valid_offset = static_cast<uint64_t>(file.tellg());

		logical_record = {};
		payload_buffer.clear();
	}

	return result;
}