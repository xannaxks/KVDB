#include "record.h"

ByteRecord::ByteRecord(const InternalRecord& entry)
	: key(entry.key), value(entry.value), type(entry.type)
{
}
ByteRecord::ByteRecord(ArenaEntry key_entry, ArenaEntry value_entry, Type type)
	: key_entry(key_entry), value_entry(value_entry), type(type)
{}

InternalRecord::InternalRecord(ArenaEntry key_entry, ArenaEntry value_entry, Type type, uint64_t seq_num)
	: key_entry(key_entry), value_entry(value_entry), type(type), seq_num(seq_num)
{}


std::vector<std::byte> InternalRecord::return_byte_sequence()
{
	std::vector<std::byte> out;
	write_raw_bytes(out, &this->key_entry.size, sizeof(this->key_entry.size));
	write_raw_bytes(out, &this->value_entry.size, sizeof(this->value_entry.size));
	write_raw_bytes(out, &this->key_entry.data, this->key_entry.size);
	write_raw_bytes(out, &this->value_entry.data, this->data_entry.size);
	return out;
}

uint32_t InternalRecord::disk_size()
{
	return key_entry.size + value_entry.size + sizeof(seq_num) + sizeof(type);
}