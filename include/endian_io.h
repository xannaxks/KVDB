#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "file.h"
#include "status.h"

namespace kvdb::endian
{
    void put_u8(std::vector<std::byte>& out, std::uint8_t value);
    void put_u16_le(std::vector<std::byte>& out, std::uint16_t value);
    void put_u32_le(std::vector<std::byte>& out, std::uint32_t value);
    void put_u64_le(std::vector<std::byte>& out, std::uint64_t value);

    // Appends a u32 little-endian size prefix followed by bytes.
    // Returns InvalidArgument when bytes.size() cannot be represented by u32.
    Status put_bytes_with_u32_size(
        std::vector<std::byte>& out,
        std::span<const std::byte> bytes
    );
}

namespace kvdb::blockio
{
    Status write_u8_t(
        WritableFile& file,
        std::uint8_t value,
        std::uint64_t& offset,
        std::uint32_t block_size
    );

    Status write_u16_t_le(
        WritableFile& file,
        std::uint16_t value,
        std::uint64_t& offset,
        std::uint32_t block_size
    );

    Status write_u32_t_le(
        WritableFile& file,
        std::uint32_t value,
        std::uint64_t& offset,
        std::uint32_t block_size
    );

    Status write_u64_t_le(
        WritableFile& file,
        std::uint64_t value,
        std::uint64_t& offset,
        std::uint32_t block_size
    );

    Status write_bytes(
        WritableFile& file,
        std::span<const std::byte> data,
        std::uint64_t& offset,
        std::uint32_t block_size
    );

    Status write_bytes_with_u32_size(
        WritableFile& file,
        std::span<const std::byte> data,
        std::uint64_t& offset,
        std::uint32_t block_size
    );

    Status read_u8_t(
        ReadableFile& file,
        std::uint8_t& value,
        std::uint64_t& offset,
        std::uint32_t block_size
    );

    Status read_u16_t_le(
        ReadableFile& file,
        std::uint16_t& value,
        std::uint64_t& offset,
        std::uint32_t block_size
    );

    Status read_u32_t_le(
        ReadableFile& file,
        std::uint32_t& value,
        std::uint64_t& offset,
        std::uint32_t block_size
    );

    Status read_u64_t_le(
        ReadableFile& file,
        std::uint64_t& value,
        std::uint64_t& offset,
        std::uint32_t block_size
    );

    Status read_bytes(
        ReadableFile& file,
        std::byte* buffer,
        std::uint32_t size,
        std::uint64_t& offset,
        std::uint32_t block_size
    );

    Status read_bytes_with_u32_size(
        ReadableFile& file,
        std::span<std::byte> buffer,
        std::uint32_t& bytes_read,
        std::uint64_t& offset,
        std::uint32_t block_size
    );
}