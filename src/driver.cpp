#include "driver.h"

bool ::VirtualNode::operator<(const ::VirtualNode& other) const
{
    if (this->key_entry == other.key_entry)
        return this->seq_number > other.seq_number; // For the same keys, the one with higher seq_number is considered "less" to ensure it comes first in the search
    return this->key_entry < other.key_entry;
}
bool ::VirtualNode::operator>(const ::VirtualNode& other) const
{
    return other < *this;
}

bool ::VirtualNode::operator==(const ::VirtualNode& other) const
{
    //return this->key_entry == other.key_entry && this->seq_number == other.seq_number && this->value_entry == other.value_entry;
    //return this->key_entry == other.key_entry && this->seq_number == other.seq_number && this->value_entry == other.value_entry;
       // Comparator equivalence and duplicate identity are key + sequence.
          // Value and type are payload and must not change node identity.
    return this->key_entry == other.key_entry &&
        this->seq_number == other.seq_number;
}

bool ::VirtualNode::operator<(const ::InternalRecord& other) const
{
	if (this->key_entry == other.key_entry)
		return this->seq_number > other.seq_num; // For the same keys, the one with higher seq_number is considered "less" to ensure it comes first in the search
	return this->key_entry < other.key_entry;
}

bool ::VirtualNode::operator>(const ::InternalRecord& other) const
{
	if (this->key_entry == other.key_entry)
		return this->seq_number < other.seq_num; // For the same keys, the one with higher seq_number is considered "less" to ensure it comes first in the search
	return  this->key_entry > other.key_entry;
}

bool ::VirtualNode::operator==(const ::InternalRecord& other) const
{
	//return this->key_entry == other.key_entry && this->seq_number == other.seq_num && this->value_entry == other.value_entry;
	//return this->key_entry == other.key_entry && this->seq_number == other.seq_num && this->value_entry == other.value_entry;
	   // Comparator equivalence and duplicate identity are key + sequence.
		  // Value and type are payload and must not change node identity.
	return this->key_entry == other.key_entry &&
		this->seq_number == other.seq_num;
}

std::size_t ::VirtualNode::approximate_memory_usage() const
{
    return sizeof(::VirtualNode) + this->key_entry.size + this->value_entry.size;
}
