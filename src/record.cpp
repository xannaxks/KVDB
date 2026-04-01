#include "record.h"

ByteRecord::ByteRecord(const InternalRecord& entry)
	: key(entry.key), value(entry.value), type(entry.type)
{
}
ByteRecord::ByteRecord(const std::string& key, const std::string& value, Type type)
	: type(type)
{
	::write_to_bytes(this->key, key);
	::write_to_bytes(this->value, value);
}
ByteRecord::ByteRecord(const Bytes& key, const Bytes& value, Type type)
	: key(key), value(value), type(type)
{
}

InternalRecord::InternalRecord(const std::string& key, const std::string& value, Type type, uint64_t seq_num)
	: type(type), seq_num(seq_num)
{
	::write_to_bytes(this->key, key);
	::write_to_bytes(this->value, value);
}
InternalRecord::InternalRecord(const Bytes& key, const Bytes& value, Type type, uint64_t seq_num)
	: key(key), value(value), type(type), seq_num(seq_num)
{
}