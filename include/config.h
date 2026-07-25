#pragma once

#include <cstdint>

inline constexpr std::uint32_t WAL_FILE_MAGIC = 0x4B565741u; // "KVWA"
inline constexpr std::uint32_t WAL_VERSION = 1u;
inline constexpr std::uint32_t WAL_FILE_BLOCK_SIZE = 4u * 1024u;
