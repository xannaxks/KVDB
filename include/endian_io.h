#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <span>
#include "file.h"
#include <vector>
#include "status.h"
#include "file_helpers.h"
#include <limits>

namespace kvdb
{
    //namespace endian
    //{
    //    // -----------------------------
    //    // Vector / buffer writes
    //    // -----------------------------

    //    void put_u8(std::vector<std::byte>& out, std::uint8_t value);
    //    void put_u16_le(std::vector<std::byte>& out, std::uint16_t value);
    //    void put_u32_le(std::vector<std::byte>& out, std::uint32_t value);
    //    void put_u64_le(std::vector<std::byte>& out, std::uint64_t value);

    //    void put_bytes_with_u32_size(
    //        std::vector<std::byte>& out,
    //        std::span<const std::byte> bytes
    //    );

    //    // -----------------------------
    //    // File writes
    //    // -----------------------------

    //    Status write_u8(WritableFile& file, std::uint8_t value);
    //    Status write_u16_le(WritableFile& file, std::uint16_t value);
    //    Status write_u32_le(WritableFile& file, std::uint32_t value);
    //    Status write_u64_le(WritableFile& file, std::uint64_t value);

    //    Status write_bytes(
    //        WritableFile& file,
    //        std::span<const std::byte> bytes
    //    );

    //    Status write_bytes_with_u32_size(
    //        WritableFile& file,
    //        std::span<const std::byte> bytes
    //    );

    //    // -----------------------------
    //    // File reads
    //    // -----------------------------

    //    Result<std::optional<std::uint8_t>> read_u8(ReadableFile& file);
    //    Result<std::optional<std::uint16_t>> read_u16_le(ReadableFile& file);
    //    Result<std::optional<std::uint32_t>> read_u32_le(ReadableFile& file);
    //    Result<std::optional<std::uint64_t>> read_u64_le(ReadableFile& file);

    //    Result<std::optional<std::vector<std::byte>>> read_bytes(
    //        ReadableFile& file,
    //        std::uint32_t size
    //    );

    //    Result<std::optional<std::vector<std::byte>>> read_bytes_with_u32_size(
    //        ReadableFile& file
    //    );

    //    // -----------------------------
    //    // Span / memory reads
    //    // Useful for decoding payloads
    //    // -----------------------------

    //    class Reader
    //    {
    //    public:
    //        explicit Reader(std::span<const std::byte> data);

    //        Result<std::optional<std::uint8_t>> read_u8();
    //        Result<std::optional<std::uint16_t>> read_u16_le();
    //        Result<std::optional<std::uint32_t>> read_u32_le();
    //        Result<std::optional<std::uint64_t>> read_u64_le();

    //        Result<std::optional<std::span<const std::byte>>> read_bytes(std::uint32_t size);

    //        Result<std::optional<std::vector<std::byte>>> read_bytes_with_u32_size();

    //        bool finished() const;
    //        std::size_t remaining() const;
    //        std::size_t position() const;

    //    private:
    //        std::span<const std::byte> data_;
    //        std::size_t pos_ = 0;
    //    };
    //}

    namespace blockio
    {
        Status write_u8_t(WritableFile& file, std::uint8_t value, std::uint64_t& offset, const std::uint32_t BLOCK_SIZE);
        Status write_u16_t_le(WritableFile& file, std::uint16_t value, std::uint64_t& offset, const std::uint32_t BLOCK_SIZE);
        Status write_u32_t_le(WritableFile& file, std::uint32_t value, std::uint64_t& offset, const std::uint32_t BLOCK_SIZE);
        Status write_u64_t_le(WritableFile& file, std::uint64_t value, std::uint64_t& offset, const std::uint32_t BLOCK_SIZE);
        Status write_bytes(WritableFile& file, std::span<std::byte> data, std::uint64_t& offset, const std::uint32_t BLOCK_SIZE);
    
        Status read_u8_t(ReadableFile& file, std::uint8_t& buff, std::uint64_t& offset, const std::uint32_t BLOCK_SIZE);
        Status read_u16_t_le(ReadableFile& file, std::uint16_t& buff, std::uint64_t& offset, const std::uint32_t BLOCK_SIZE);
        Status read_u32_t_le(ReadableFile& file, std::uint32_t& buff, std::uint64_t& offset, const std::uint32_t BLOCK_SIZE);
        Status read_u64_t_le(ReadableFile& file, std::uint64_t& buff, std::uint64_t& offset, const std::uint32_t BLOCK_SIZE);

        Status read_bytes(
            ReadableFile& file,
            std::byte* buf,
            std::uint32_t size,
            std::uint64_t& offset,
            const std::uint32_t BLOCK_SIZE
        );

        Status read_bytes_with_u32_size(
            ReadableFile& file,
            std::span<std::byte> buf,
            std::uint32_t& bytes_read,
            std::uint64_t& offset,
            const std::uint32_t BLOCK_SIZE
        );
    
    }
}
