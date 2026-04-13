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
