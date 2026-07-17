#include "endian_io.h"

#include <array>
#include <format>
#include <limits>
#include <utility>

#include "file_helpers.h"

namespace
{
    Status validate_block_size(std::uint32_t block_size)
    {
        if (block_size == 0) {
            return Status{
                StatusCode::InvalidArgument,
                "block size must be greater than zero"
            };
        }

        return Status::ok();
    }

    Status check_writer_offset(WritableFile& file, std::uint64_t offset)
    {
        Result<std::uint64_t> position = file.current_position();
        if (!position.is_ok()) {
            return std::move(position.status);
        }

        if (position.value != offset) {
            return Status{
                StatusCode::InvalidOffset,
                std::format(
                    "writable file cursor {} and tracked offset {} differ",
                    position.value,
                    offset
                )
            };
        }

        return Status::ok();
    }

    Status prepare_write(
        WritableFile& file,
        std::size_t size,
        std::uint64_t& offset,
        std::uint32_t block_size
    )
    {
        Status status = validate_block_size(block_size);
        if (!status.is_ok()) {
            return status;
        }

        status = check_writer_offset(file, offset);
        if (!status.is_ok()) {
            return status;
        }

        return ensure_fits_in_block(file, size, offset, block_size);
    }
}

namespace kvdb::endian
{
    void put_u8(std::vector<std::byte>& out, std::uint8_t value)
    {
        out.push_back(static_cast<std::byte>(value));
    }

    void put_u16_le(std::vector<std::byte>& out, std::uint16_t value)
    {
        out.push_back(static_cast<std::byte>(value & 0xFFu));
        out.push_back(static_cast<std::byte>((value >> 8u) & 0xFFu));
    }

    void put_u32_le(std::vector<std::byte>& out, std::uint32_t value)
    {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
        }
    }

    void put_u64_le(std::vector<std::byte>& out, std::uint64_t value)
    {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            out.push_back(static_cast<std::byte>((value >> shift) & 0xFFull));
        }
    }

    Status put_bytes_with_u32_size(
        std::vector<std::byte>& out,
        std::span<const std::byte> bytes
    )
    {
        if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
            return Status{
                StatusCode::InvalidArgument,
                "byte sequence size cannot be represented by u32"
            };
        }

        put_u32_le(out, static_cast<std::uint32_t>(bytes.size()));
        out.insert(out.end(), bytes.begin(), bytes.end());
        return Status::ok();
    }
}

namespace kvdb::blockio
{
    Status write_u8_t(
        WritableFile& file,
        std::uint8_t value,
        std::uint64_t& offset,
        std::uint32_t block_size
    )
    {
        Status status = prepare_write(file, sizeof(value), offset, block_size);
        if (!status.is_ok()) {
            return status;
        }

        return file.append(&value, sizeof(value), offset);
    }

    Status write_u16_t_le(
        WritableFile& file,
        std::uint16_t value,
        std::uint64_t& offset,
        std::uint32_t block_size
    )
    {
        Status status = prepare_write(file, sizeof(value), offset, block_size);
        if (!status.is_ok()) {
            return status;
        }

        const std::array<std::uint8_t, sizeof(value)> encoded{
            static_cast<std::uint8_t>(value & 0xFFu),
            static_cast<std::uint8_t>((value >> 8u) & 0xFFu)
        };

        return file.append(encoded.data(), encoded.size(), offset);
    }

    Status write_u32_t_le(
        WritableFile& file,
        std::uint32_t value,
        std::uint64_t& offset,
        std::uint32_t block_size
    )
    {
        Status status = prepare_write(file, sizeof(value), offset, block_size);
        if (!status.is_ok()) {
            return status;
        }

        const std::array<std::uint8_t, sizeof(value)> encoded{
            static_cast<std::uint8_t>(value & 0xFFu),
            static_cast<std::uint8_t>((value >> 8u) & 0xFFu),
            static_cast<std::uint8_t>((value >> 16u) & 0xFFu),
            static_cast<std::uint8_t>((value >> 24u) & 0xFFu)
        };

        return file.append(encoded.data(), encoded.size(), offset);
    }

    Status write_u64_t_le(
        WritableFile& file,
        std::uint64_t value,
        std::uint64_t& offset,
        std::uint32_t block_size
    )
    {
        Status status = prepare_write(file, sizeof(value), offset, block_size);
        if (!status.is_ok()) {
            return status;
        }

        const std::array<std::uint8_t, sizeof(value)> encoded{
            static_cast<std::uint8_t>(value & 0xFFull),
            static_cast<std::uint8_t>((value >> 8u) & 0xFFull),
            static_cast<std::uint8_t>((value >> 16u) & 0xFFull),
            static_cast<std::uint8_t>((value >> 24u) & 0xFFull),
            static_cast<std::uint8_t>((value >> 32u) & 0xFFull),
            static_cast<std::uint8_t>((value >> 40u) & 0xFFull),
            static_cast<std::uint8_t>((value >> 48u) & 0xFFull),
            static_cast<std::uint8_t>((value >> 56u) & 0xFFull)
        };

        return file.append(encoded.data(), encoded.size(), offset);
    }

    Status write_bytes(
        WritableFile& file,
        std::span<const std::byte> data,
        std::uint64_t& offset,
        std::uint32_t block_size
    )
    {
        Status status = prepare_write(file, data.size_bytes(), offset, block_size);
        if (!status.is_ok()) {
            return status;
        }

        if (data.empty()) {
            return Status::ok();
        }

        return file.append(data.data(), data.size_bytes(), offset);
    }

    Status write_bytes_with_u32_size(
        WritableFile& file,
        std::span<const std::byte> data,
        std::uint64_t& offset,
        std::uint32_t block_size
    )
    {
        Status status = validate_block_size(block_size);
        if (!status.is_ok()) {
            return status;
        }

        if (data.size() > std::numeric_limits<std::uint32_t>::max()) {
            return Status{
                StatusCode::InvalidArgument,
                "byte sequence size cannot be represented by u32"
            };
        }

        // Preflight this before writing the prefix. Otherwise an oversized payload
        // would leave a partial sized-field record in the file.
        if (data.size_bytes() > block_size) {
            return Status{
                StatusCode::SizeExceedsBlockSize,
                "byte sequence exceeds block size"
            };
        }

        status = check_writer_offset(file, offset);
        if (!status.is_ok()) {
            return status;
        }

        status = write_u32_t_le(
            file,
            static_cast<std::uint32_t>(data.size()),
            offset,
            block_size
        );
        if (!status.is_ok()) {
            return status;
        }

        return write_bytes(file, data, offset, block_size);
    }

    Status read_u8_t(
        ReadableFile& file,
        std::uint8_t& value,
        std::uint64_t& offset,
        std::uint32_t block_size
    )
    {
        Status status = validate_block_size(block_size);
        if (!status.is_ok()) {
            return status;
        }

        const std::uint64_t initial_offset = offset;
        std::uint8_t encoded = 0;

        status = ensure_fits_in_block(file, sizeof(encoded), offset, block_size);
        if (!status.is_ok()) {
            offset = initial_offset;
            return status;
        }

        status = file.read_exact_at(offset, &encoded, sizeof(encoded), offset);
        if (!status.is_ok()) {
            offset = initial_offset;
            return status;
        }

        value = encoded;
        return Status::ok();
    }

    Status read_u16_t_le(
        ReadableFile& file,
        std::uint16_t& value,
        std::uint64_t& offset,
        std::uint32_t block_size
    )
    {
        Status status = validate_block_size(block_size);
        if (!status.is_ok()) {
            return status;
        }

        const std::uint64_t initial_offset = offset;
        std::array<std::uint8_t, sizeof(value)> encoded{};

        status = ensure_fits_in_block(file, encoded.size(), offset, block_size);
        if (!status.is_ok()) {
            offset = initial_offset;
            return status;
        }

        status = file.read_exact_at(offset, encoded.data(), encoded.size(), offset);
        if (!status.is_ok()) {
            offset = initial_offset;
            return status;
        }

        value =
            static_cast<std::uint16_t>(encoded[0]) |
            (static_cast<std::uint16_t>(encoded[1]) << 8u);

        return Status::ok();
    }

    Status read_u32_t_le(
        ReadableFile& file,
        std::uint32_t& value,
        std::uint64_t& offset,
        std::uint32_t block_size
    )
    {
        Status status = validate_block_size(block_size);
        if (!status.is_ok()) {
            return status;
        }

        const std::uint64_t initial_offset = offset;
        std::array<std::uint8_t, sizeof(value)> encoded{};

        status = ensure_fits_in_block(file, encoded.size(), offset, block_size);
        if (!status.is_ok()) {
            offset = initial_offset;
            return status;
        }

        status = file.read_exact_at(offset, encoded.data(), encoded.size(), offset);
        if (!status.is_ok()) {
            offset = initial_offset;
            return status;
        }

        value =
            static_cast<std::uint32_t>(encoded[0]) |
            (static_cast<std::uint32_t>(encoded[1]) << 8u) |
            (static_cast<std::uint32_t>(encoded[2]) << 16u) |
            (static_cast<std::uint32_t>(encoded[3]) << 24u);

        return Status::ok();
    }

    Status read_u64_t_le(
        ReadableFile& file,
        std::uint64_t& value,
        std::uint64_t& offset,
        std::uint32_t block_size
    )
    {
        Status status = validate_block_size(block_size);
        if (!status.is_ok()) {
            return status;
        }

        const std::uint64_t initial_offset = offset;
        std::array<std::uint8_t, sizeof(value)> encoded{};

        status = ensure_fits_in_block(file, encoded.size(), offset, block_size);
        if (!status.is_ok()) {
            offset = initial_offset;
            return status;
        }

        status = file.read_exact_at(offset, encoded.data(), encoded.size(), offset);
        if (!status.is_ok()) {
            offset = initial_offset;
            return status;
        }

        value =
            static_cast<std::uint64_t>(encoded[0]) |
            (static_cast<std::uint64_t>(encoded[1]) << 8u) |
            (static_cast<std::uint64_t>(encoded[2]) << 16u) |
            (static_cast<std::uint64_t>(encoded[3]) << 24u) |
            (static_cast<std::uint64_t>(encoded[4]) << 32u) |
            (static_cast<std::uint64_t>(encoded[5]) << 40u) |
            (static_cast<std::uint64_t>(encoded[6]) << 48u) |
            (static_cast<std::uint64_t>(encoded[7]) << 56u);

        return Status::ok();
    }

    Status read_bytes(
        ReadableFile& file,
        std::byte* buffer,
        std::uint32_t size,
        std::uint64_t& offset,
        std::uint32_t block_size
    )
    {
        Status status = validate_block_size(block_size);
        if (!status.is_ok()) {
            return status;
        }

        if (size == 0) {
            return Status::ok();
        }

        if (buffer == nullptr) {
            return Status{
                StatusCode::InvalidArgument,
                "read_bytes received a null buffer for a non-zero read"
            };
        }

        const std::uint64_t initial_offset = offset;

        status = ensure_fits_in_block(file, size, offset, block_size);
        if (!status.is_ok()) {
            offset = initial_offset;
            return status;
        }

        status = file.read_exact_at(offset, buffer, size, offset);
        if (!status.is_ok()) {
            offset = initial_offset;
            return status;
        }

        return Status::ok();
    }

    Status read_bytes_with_u32_size(
        ReadableFile& file,
        std::span<std::byte> buffer,
        std::uint32_t& bytes_read,
        std::uint64_t& offset,
        std::uint32_t block_size
    )
    {
        Status status = validate_block_size(block_size);
        if (!status.is_ok()) {
            return status;
        }

        const std::uint64_t initial_offset = offset;
        std::uint32_t encoded_size = 0;

        status = read_u32_t_le(file, encoded_size, offset, block_size);
        if (!status.is_ok()) {
            offset = initial_offset;
            return status;
        }

        if (encoded_size > buffer.size_bytes()) {
            offset = initial_offset;
            return Status{
                StatusCode::BufferTooSmall,
                "destination buffer is smaller than encoded byte sequence"
            };
        }

        status = read_bytes(file, buffer.data(), encoded_size, offset, block_size);
        if (!status.is_ok()) {
            offset = initial_offset;
            return status;
        }

        bytes_read = encoded_size;
        return Status::ok();
    }
}