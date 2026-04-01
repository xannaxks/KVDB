#include "bytes.h"
#include <string>

void write_to_bytes(Bytes& bytes, const std::string& value)
{
    uint64_t size = value.size();
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(value.data());
    bytes.insert(bytes.end(), ptr, ptr + size);
}

void write_to_string(std::string& buffer, const Bytes& bytes)
{
    buffer.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}