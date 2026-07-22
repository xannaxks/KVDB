#include "crc32_helpers.h"

#include <algorithm>
#include <cassert>
#include <limits>


void compute_crc32(
    std::uint32_t& crc,
    const void* data,
    std::size_t size
) noexcept
{
    if (size == 0)
        return;

    assert(data != nullptr);

    if (data == nullptr)
        return;

    const auto* current = static_cast<const Bytef*>(data);
    uLong zlib_crc = static_cast<uLong>(crc);

    /*
        * zlib's crc32() accepts uInt for the length, while size_t can be larger.
        * Process very large buffers in chunks.
        */
    while (size > 0)
    {
        const std::size_t chunk_size = std::min(
            size,
            static_cast<std::size_t>(
                std::numeric_limits<uInt>::max()
                )
        );

        zlib_crc = ::crc32(
            zlib_crc,
            current,
            static_cast<uInt>(chunk_size)
        );

        current += chunk_size;
        size -= chunk_size;
    }

    crc = static_cast<std::uint32_t>(zlib_crc);
}

std::uint32_t crc32_of(
    const void* data,
    std::size_t size
) noexcept
{
    auto crc = static_cast<std::uint32_t>(
        ::crc32(0L, Z_NULL, 0)
        );

    compute_crc32(crc, data, size);
    return crc;
}
