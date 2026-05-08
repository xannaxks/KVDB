#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <span>
#include <vector>

namespace kvdb::endian
{
    // -----------------------------
    // Vector / buffer writes
    // -----------------------------

    void put_u8(std::vector<std::byte>& out, uint8_t value);
    void put_u16_le(std::vector<std::byte>& out, uint16_t value);
    void put_u32_le(std::vector<std::byte>& out, uint32_t value);
    void put_u64_le(std::vector<std::byte>& out, uint64_t value);

    void put_bytes_with_u32_size(
        std::vector<std::byte>& out,
        std::span<const std::byte> bytes
    );

    // -----------------------------
    // File writes
    // -----------------------------

    bool write_u8(std::ofstream& file, uint8_t value);
    bool write_u16_le(std::ofstream& file, uint16_t value);
    bool write_u32_le(std::ofstream& file, uint32_t value);
    bool write_u64_le(std::ofstream& file, uint64_t value);

    bool write_bytes(
        std::ofstream& file,
        std::span<const std::byte> bytes
    );

    bool write_bytes_with_u32_size(
        std::ofstream& file,
        std::span<const std::byte> bytes
    );

    // -----------------------------
    // File reads
    // -----------------------------

    std::optional<uint8_t> read_u8(std::ifstream& file);
    std::optional<uint16_t> read_u16_le(std::ifstream& file);
    std::optional<uint32_t> read_u32_le(std::ifstream& file);
    std::optional<uint64_t> read_u64_le(std::ifstream& file);

    std::optional<std::vector<std::byte>> read_bytes(
        std::ifstream& file,
        uint32_t size
    );

    std::optional<std::vector<std::byte>> read_bytes_with_u32_size(
        std::ifstream& file
    );

    // -----------------------------
    // Span / memory reads
    // Useful for decoding payloads
    // -----------------------------

    class Reader
    {
    public:
        explicit Reader(std::span<const std::byte> data);

        std::optional<uint8_t> read_u8();
        std::optional<uint16_t> read_u16_le();
        std::optional<uint32_t> read_u32_le();
        std::optional<uint64_t> read_u64_le();

        std::optional<std::span<const std::byte>> read_bytes(uint32_t size);

        std::optional<std::vector<std::byte>> read_bytes_with_u32_size();

        bool finished() const;
        std::size_t remaining() const;
        std::size_t position() const;

    private:
        std::span<const std::byte> data_;
        std::size_t pos_ = 0;
    };
}