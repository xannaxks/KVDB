#pragma once

#include <vector>
#include <string>

using Bytes = std::vector<uint8_t>;
void write_to_bytes(Bytes& bytes, const std::string& value);
void write_to_string(std::string& buffer, const Bytes& bytes);