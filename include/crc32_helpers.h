#pragma once

#include <array>
#include <zlib.h>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include "endian_io.h"

inline void init_crc_buff(std::uint32_t& crc)
{
    static_cast<std::uint32_t>(
        ::crc32(0L, Z_NULL, 0)
        );
}

// Implemented in crc32_helpers.cpp.
// This updates the numeric CRC state using exactly the supplied bytes.
void compute_crc32(
    std::uint32_t& crc,
    const void* data,
    std::size_t size
) noexcept;

[[nodiscard]]
std::uint32_t crc32_of(
    const void* data,
    std::size_t size
) noexcept;

inline void compute_span_crc32(
    std::uint32_t& crc,
    std::span<const std::byte> value
) noexcept
{
    compute_crc32(crc, value.data(), value.size());
}

/*
    * Adds an unsigned integer to the CRC in canonical little-endian encoding.
    *
    * For example, 0x12345678 is always hashed as:
    *     78 56 34 12
    *
    * regardless of host endianness.
    */
template <std::unsigned_integral T>
    requires (sizeof(T) <= sizeof(std::uint64_t))
inline void crc32_add_pod(
    std::uint32_t& crc,
    T value
) noexcept
{
    std::array<std::byte, sizeof(T)> encoded{};

    const auto wide_value = static_cast<std::uint64_t>(value);

    for (std::size_t i = 0; i < encoded.size(); ++i)
    {
        encoded[i] = static_cast<std::byte>(
            (wide_value >> (i * 8u)) & 0xFFu
            );
    }

    compute_crc32(crc, encoded.data(), encoded.size());
}

inline void crc32_add_bytes(
    std::uint32_t& crc,
    std::span<const std::byte> bytes
) noexcept
{
    compute_crc32(crc, bytes.data(), bytes.size());
}

inline void crc32_add_string(
    std::uint32_t& crc,
    std::string_view value
) noexcept
{
    compute_crc32(crc, value.data(), value.size());
}

/*
    * Hashes the same representation as:
    *
    *     put_u32(output, value.size());
    *     put_bytes(output, value.data(), value.size());
    */
[[nodiscard]]
inline bool crc32_add_string_with_u32_size(
    std::uint32_t& crc,
    std::string_view value
) noexcept
{
    if (value.size() > std::numeric_limits<std::uint32_t>::max())
        return false;

    crc32_add_pod(
        crc,
        static_cast<std::uint32_t>(value.size())
    );

    crc32_add_string(crc, value);
    return true;
}

/*
    * Stores the completed numeric CRC as little-endian bytes.
    *
    * Do not byte-swap the CRC while it is being updated. Cosnvert it only when
    * serializing it.
    */
inline void put_crc32_le(
    std::vector<std::byte>& output,
    std::uint32_t crc
)
{
    kvdb::endian::put_u32_le(output, crc);
}
