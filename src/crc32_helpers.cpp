#include "crc32_helpers.h"

void compute_crc32(uint32_t& crc, const void* ptr, std::size_t size)
{
    // TEMPORARY PLACEHOLDER.
    // This is NOT real CRC32.
    // It is only for keeping serialization/tests/build working
    // until zlib or a real CRC32 implementation is added.

    if (ptr == nullptr || size == 0) return;

    const auto* bytes = static_cast<const std::byte*>(ptr);

    for (std::size_t i = 0; i < size; ++i)
    {
        crc = crc * 31u + static_cast<uint32_t>(bytes[i]);
    }
}

inline uint32_t crc32_of(const void* ptr, std::size_t size)
{
    uint32_t crc = 0;
    crc = ::crc32(0L, Z_NULL, 0);
    compute_crc32(crc, ptr, size);
    return crc;
}

inline void compute_span_crc32(uint32_t& crc, const std::span<const std::byte>& value)
{
    compute_crc32(crc, value.data(), value.size());
}