#include "endian_io.h"
#include <format>
#include <cassert>

namespace kvdb
{
    //namespace endian {
    //    void put_u8(std::vector<std::byte>& out, std::uint8_t value)
    //    {
    //        out.push_back(static_cast<std::byte>(value));
    //    }
    //    void put_u16_le(std::vector<std::byte>& out, std::uint16_t value)
    //    {
    //        out.push_back(static_cast<std::byte>((value >> 0) & 0xFF));
    //        out.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
    //    }
    //    void put_u32_le(std::vector<std::byte>& out, std::uint32_t value)
    //    {
    //        for (int shift = 0; shift < 32; shift += 8)
    //        {
    //            out.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
    //        }
    //    }
    //    void put_u64_le(std::vector<std::byte>& out, std::uint64_t value)
    //    {
    //        for (int shift = 0; shift < 64; shift += 8)
    //        {
    //            out.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
    //        }
    //    }
    //    void put_bytes_with_u32_size(
    //        std::vector<std::byte>& out,
    //        std::span<const std::byte> bytes
    //    )
    //    {
    //        // For your KVDB keys/values, u32 size is probably enough.
    //        // If you want values > 4GB later, switch to u64.
    //        put_u32_le(out, static_cast<std::uint32_t>(bytes.size()));
    //        out.insert(out.end(), bytes.begin(), bytes.end());
    //    }


    //    Status write_bytes(
    //        WritableFile& file,
    //        std::span<const std::byte> bytes
    //    )
    //    {
    //        if (!bytes.empty())
    //        {
    //            file.write(
    //                reinterpret_cast<const char*>(bytes.data()),
    //                static_cast<std::streamsize>(bytes.size())
    //            );
    //        }

    //        return file.good();
    //    }
    //    Status write_u8(WritableFile& file, std::uint8_t value)
    //    {
    //        char byte = static_cast<char>(value);
    //        file.write(&byte, 1);
    //        return file.good();
    //    }
    //    Status write_u16_le(WritableFile& file, std::uint16_t value)
    //    {
    //        std::array<std::byte, 2> bytes{
    //            static_cast<std::byte>((value >> 0) & 0xFF),
    //            static_cast<std::byte>((value >> 8) & 0xFF)
    //        };

    //        file.write(
    //            reinterpret_cast<const char*>(bytes.data()),
    //            static_cast<std::streamsize>(bytes.size())
    //        );

    //        return file.good();
    //    }
    //    Status write_u32_le(WritableFile& file, std::uint32_t value)
    //    {
    //        std::array<std::byte, 4> bytes{
    //            static_cast<std::byte>((value >> 0) & 0xFF),
    //            static_cast<std::byte>((value >> 8) & 0xFF),
    //            static_cast<std::byte>((value >> 16) & 0xFF),
    //            static_cast<std::byte>((value >> 24) & 0xFF)
    //        };

    //        file.write(
    //            reinterpret_cast<const char*>(bytes.data()),
    //            static_cast<std::streamsize>(bytes.size())
    //        );

    //        return file.good();
    //    }
    //    Status write_u64_le(WritableFile& file, std::uint64_t value)
    //    {
    //        std::array<std::byte, 8> bytes{
    //            static_cast<std::byte>((value >> 0) & 0xFF),
    //            static_cast<std::byte>((value >> 8) & 0xFF),
    //            static_cast<std::byte>((value >> 16) & 0xFF),
    //            static_cast<std::byte>((value >> 24) & 0xFF),
    //            static_cast<std::byte>((value >> 32) & 0xFF),
    //            static_cast<std::byte>((value >> 40) & 0xFF),
    //            static_cast<std::byte>((value >> 48) & 0xFF),
    //            static_cast<std::byte>((value >> 56) & 0xFF)
    //        };

    //        file.write(
    //            reinterpret_cast<const char*>(bytes.data()),
    //            static_cast<std::streamsize>(bytes.size())
    //        );

    //        return file.good();
    //    }
    //    Status write_bytes_with_u32_size(
    //        WritableFile& file,
    //        std::span<const std::byte> bytes
    //    )
    //    {
    //        if (bytes.size() > std::numeric_limits<std::uint32_t>::max())
    //        {
    //            return false;
    //        }

    //        if (!write_u32_le(file, static_cast<std::uint32_t>(bytes.size())))
    //        {
    //            return false;
    //        }

    //        return write_bytes(file, bytes);
    //    }

    //    Result<std::optional<std::uint8_t>> read_u8(ReadableFile& file)
    //    {
    //        char byte = 0;
    //        file.read(&byte, 1);

    //        if (file.gcount() != 1)
    //        {
    //            return std::nullopt;
    //        }

    //        return static_cast<std::uint8_t>(static_cast<unsigned char>(byte));
    //    }
    //    Result<std::optional<std::uint16_t>> read_u16_le(ReadableFile& file)
    //    {
    //        std::array<unsigned char, 2> bytes{};

    //        file.read(reinterpret_cast<char*>(bytes.data()), 2);

    //        if (file.gcount() != 2)
    //        {
    //            return std::nullopt;
    //        }

    //        std::uint16_t value = 0;

    //        value |= static_cast<std::uint16_t>(bytes[0]) << 0;
    //        value |= static_cast<std::uint16_t>(bytes[1]) << 8;

    //        return value;
    //    }
    //    Result<std::optional<std::uint32_t>> read_u32_le(ReadableFile& file)
    //    {
    //        std::array<unsigned char, 4> bytes{};

    //        file.read(reinterpret_cast<char*>(bytes.data()), 4);

    //        if (file.gcount() != 4)
    //        {
    //            return std::nullopt;
    //        }

    //        std::uint32_t value = 0;

    //        value |= static_cast<std::uint32_t>(bytes[0]) << 0;
    //        value |= static_cast<std::uint32_t>(bytes[1]) << 8;
    //        value |= static_cast<std::uint32_t>(bytes[2]) << 16;
    //        value |= static_cast<std::uint32_t>(bytes[3]) << 24;

    //        return value;
    //    }
    //    Result<std::optional<std::uint64_t>> read_u64_le(ReadableFile& file)
    //    {
    //        std::array<unsigned char, 8> bytes{};

    //        file.read(reinterpret_cast<char*>(bytes.data()), 8);

    //        if (file.gcount() != 8)
    //        {
    //            return std::nullopt;
    //        }

    //        std::uint64_t value = 0;

    //        value |= static_cast<std::uint64_t>(bytes[0]) << 0;
    //        value |= static_cast<std::uint64_t>(bytes[1]) << 8;
    //        value |= static_cast<std::uint64_t>(bytes[2]) << 16;
    //        value |= static_cast<std::uint64_t>(bytes[3]) << 24;
    //        value |= static_cast<std::uint64_t>(bytes[4]) << 32;
    //        value |= static_cast<std::uint64_t>(bytes[5]) << 40;
    //        value |= static_cast<std::uint64_t>(bytes[6]) << 48;
    //        value |= static_cast<std::uint64_t>(bytes[7]) << 56;

    //        return value;
    //    }
    //    Result<std::optional<std::vector<std::byte>>> read_bytes(
    //        ReadableFile& file,
    //        std::uint32_t size
    //    )
    //    {
    //        std::vector<std::byte> bytes(size);

    //        if (size == 0)
    //        {
    //            return bytes;
    //        }

    //        file.read(
    //            reinterpret_cast<char*>(bytes.data()),
    //            static_cast<std::streamsize>(size)
    //        );

    //        if (static_cast<std::uint32_t>(file.gcount()) != size)
    //        {
    //            return std::nullopt;
    //        }

    //        return bytes;
    //    }
    //    Result<std::optional<std::vector<std::byte>>> read_bytes_with_u32_size(
    //        ReadableFile& file
    //    )
    //    {
    //        auto size = read_u32_le(file);

    //        if (!size.has_value())
    //        {
    //            return std::nullopt;
    //        }

    //        return read_bytes(file, *size);
    //    }


    //    Reader::Reader(std::span<const std::byte> data)
    //        : data_(data)
    //    {
    //    }

    //    Result<std::optional<std::uint8_t>> Reader::read_u8()
    //    {
    //        if (remaining() < 1)
    //        {
    //            return std::nullopt;
    //        }

    //        std::uint8_t value = static_cast<std::uint8_t>(data_[pos_]);
    //        ++pos_;

    //        return value;
    //    }
    //    Result<std::optional<std::uint16_t>> Reader::read_u16_le()
    //    {
    //        if (remaining() < 2)
    //        {
    //            return std::nullopt;
    //        }

    //        std::uint16_t value = 0;

    //        value |= static_cast<std::uint16_t>(data_[pos_ + 0]) << 0;
    //        value |= static_cast<std::uint16_t>(data_[pos_ + 1]) << 8;

    //        pos_ += 2;

    //        return value;
    //    }
    //    Result<std::optional<std::uint32_t>> Reader::read_u32_le()
    //    {
    //        if (remaining() < 4)
    //        {
    //            return std::nullopt;
    //        }

    //        std::uint32_t value = 0;

    //        value |= static_cast<std::uint32_t>(data_[pos_ + 0]) << 0;
    //        value |= static_cast<std::uint32_t>(data_[pos_ + 1]) << 8;
    //        value |= static_cast<std::uint32_t>(data_[pos_ + 2]) << 16;
    //        value |= static_cast<std::uint32_t>(data_[pos_ + 3]) << 24;

    //        pos_ += 4;

    //        return value;
    //    }
    //    Result<std::optional<std::uint64_t>> Reader::read_u64_le()
    //    {
    //        if (remaining() < 8)
    //        {
    //            return std::nullopt;
    //        }

    //        std::uint64_t value = 0;

    //        value |= static_cast<std::uint64_t>(data_[pos_ + 0]) << 0;
    //        value |= static_cast<std::uint64_t>(data_[pos_ + 1]) << 8;
    //        value |= static_cast<std::uint64_t>(data_[pos_ + 2]) << 16;
    //        value |= static_cast<std::uint64_t>(data_[pos_ + 3]) << 24;
    //        value |= static_cast<std::uint64_t>(data_[pos_ + 4]) << 32;
    //        value |= static_cast<std::uint64_t>(data_[pos_ + 5]) << 40;
    //        value |= static_cast<std::uint64_t>(data_[pos_ + 6]) << 48;
    //        value |= static_cast<std::uint64_t>(data_[pos_ + 7]) << 56;

    //        pos_ += 8;

    //        return value;
    //    }
    //    Result<std::optional<std::span<const std::byte>>> Reader::read_bytes(std::uint32_t size)
    //    {
    //        if (remaining() < size)
    //        {
    //            return std::nullopt;
    //        }

    //        auto result = data_.subspan(pos_, size);
    //        pos_ += size;

    //        return result;
    //    }
    //    Result<std::optional<std::vector<std::byte>>> Reader::read_bytes_with_u32_size()
    //    {
    //        auto size = read_u32_le();

    //        if (!size.has_value())
    //        {
    //            return std::nullopt;
    //        }

    //        auto bytes = read_bytes(*size);

    //        if (!bytes.has_value())
    //        {
    //            return std::nullopt;
    //        }

    //        return std::vector<std::byte>(bytes->begin(), bytes->end());
    //    }

    //    bool Reader::finished() const
    //    {
    //        return pos_ == data_.size();
    //    }
    //    std::size_t Reader::remaining() const
    //    {
    //        return data_.size() - pos_;
    //    }
    //    std::size_t Reader::position() const
    //    {
    //        return pos_;
    //    }
    //}
    namespace blockio
    {

        Status write_u8_t(WritableFile& file, std::uint8_t value, std::uint64_t& offset, const std::uint32_t BLOCK_SIZE)
        {
            Result<std::uint64_t> current_position_result = file.current_position();
            if (!current_position_result.is_ok())
                return std::move(current_position_result.status);

            assert(offset == current_position_result.value);
                
            Status result = ensure_fits_in_block(file, static_cast<std::size_t>(sizeof(value)), offset, BLOCK_SIZE);
            if (!result.is_ok())
                return result;

			return file.append(&value, sizeof(value), offset);
        }
        Status write_u16_t_le(WritableFile& file, std::uint16_t value, std::uint64_t& offset, std::uint32_t block_size)
        {
            Result<std::uint64_t> position_result = file.current_position();
            if (!position_result.is_ok())
                return std::move(position_result.status);

            assert(position_result.value == offset);

            Status status = ensure_fits_in_block(
                file,
                sizeof(value),
                offset,
                block_size
            );

            if (!status.is_ok())
                return status;

            const std::array<std::uint8_t, sizeof(value)> encoded{
                static_cast<std::uint8_t>(value & 0xFFu),
                static_cast<std::uint8_t>((value >> 8u) & 0xFFu)
            };

            status = file.append(reinterpret_cast<const void*>(encoded.data()), encoded.size(), offset);
            if (!status.is_ok())
                return status;

            return Status::ok();
        }
        Status write_u32_t_le(WritableFile& file, std::uint32_t value, std::uint64_t& offset, std::uint32_t block_size)
        {
            Result<std::uint64_t> position_result = file.current_position();
            if (!position_result.is_ok())
                return std::move(position_result.status);

            assert(position_result.value == offset);

            Status status = ensure_fits_in_block(
                file,
                sizeof(value),
                offset,
                block_size
            );

            if (!status.is_ok())
                return status;

            const std::array<std::uint8_t, sizeof(value)> encoded{
                static_cast<std::uint8_t>(value & 0xFFu),
                static_cast<std::uint8_t>((value >> 8u) & 0xFFu),
                static_cast<std::uint8_t>((value >> 16u) & 0xFFu),
                static_cast<std::uint8_t>((value >> 24u) & 0xFFu)
            };

            status = file.append(
                reinterpret_cast<const void*>(encoded.data()),
                encoded.size(),
                offset
            );

            if (!status.is_ok())
                return status;

            return Status::ok();
        }
        Status write_u64_t_le(WritableFile& file, std::uint64_t value, std::uint64_t& offset, std::uint32_t block_size)
        {
            Result<std::uint64_t> position_result = file.current_position();
            if (!position_result.is_ok())
                return std::move(position_result.status);

            assert(position_result.value == offset);

            Status status = ensure_fits_in_block(
                file,
                sizeof(value),
                offset,
                block_size
            );

            if (!status.is_ok())
                return status;

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

            status = file.append(
                reinterpret_cast<const void*>(encoded.data()),
                encoded.size(),
                offset
            );

            if (!status.is_ok())
                return status;

            return Status::ok();
        }
        Status write_bytes(WritableFile& file, std::span<std::byte> data, std::uint64_t& offset, std::uint32_t block_size)
        {
            Result<std::uint64_t> position_result = file.current_position();
            if (!position_result.is_ok())
                return std::move(position_result.status);

            assert(position_result.value == offset);

            Status status = ensure_fits_in_block(
                file,
                data.size_bytes(),
                offset,
                block_size
            );

            if (!status.is_ok())
                return status;

            if (data.empty())
                return Status::ok();

            status = file.append(
                reinterpret_cast<const void*>(data.data()),
                data.size_bytes(),
                offset
            );

            if (!status.is_ok())
                return status;

            return Status::ok();
        }

        Status read_u8_t(ReadableFile& file, std::uint8_t& buff, std::uint64_t& offset, const std::uint32_t BLOCK_SIZE)
        {
            Status result = ensure_fits_in_block(file, sizeof(buff), offset, BLOCK_SIZE);
            if (!result.is_ok())
                return result;

            result = file.read_exact_at(offset, reinterpret_cast<void*>(&buff), sizeof(buff), offset);

            return result;  
        }
        Status read_u16_t_le(
            ReadableFile& file,
            std::uint16_t& buff,
            std::uint64_t& offset,
            const std::uint32_t BLOCK_SIZE
        )
        {
            std::array<std::uint8_t, sizeof(std::uint16_t)> encoded{};

            Status status = ensure_fits_in_block(
                file,
                encoded.size(),
                offset,
                BLOCK_SIZE
            );

            if (!status.is_ok())
                return status;

            status = file.read_exact_at(
                offset,
                static_cast<void*>(encoded.data()),
                encoded.size(),
                offset
            );

            if (!status.is_ok())
                return status;

            buff =
                static_cast<std::uint16_t>(encoded[0]) |
                (static_cast<std::uint16_t>(encoded[1]) << 8u);

            return Status::ok();
        }


        Status read_u32_t_le(
            ReadableFile& file,
            std::uint32_t& buff,
            std::uint64_t& offset,
            const std::uint32_t BLOCK_SIZE
        )
        {
            std::array<std::uint8_t, sizeof(std::uint32_t)> encoded{};

            Status status = ensure_fits_in_block(
                file,
                encoded.size(),
                offset,
                BLOCK_SIZE
            );

            if (!status.is_ok())
                return status;

            status = file.read_exact_at(
                offset,
                static_cast<void*>(encoded.data()),
                encoded.size(),
                offset
            );

            if (!status.is_ok())
                return status;

            buff =
                static_cast<std::uint32_t>(encoded[0]) |
                (static_cast<std::uint32_t>(encoded[1]) << 8u) |
                (static_cast<std::uint32_t>(encoded[2]) << 16u) |
                (static_cast<std::uint32_t>(encoded[3]) << 24u);

            return Status::ok();
        }


        Status read_u64_t_le(
            ReadableFile& file,
            std::uint64_t& buff,
            std::uint64_t& offset,
            const std::uint32_t BLOCK_SIZE
        )
        {
            std::array<std::uint8_t, sizeof(std::uint64_t)> encoded{};

            Status status = ensure_fits_in_block(
                file,
                encoded.size(),
                offset,
                BLOCK_SIZE
            );

            if (!status.is_ok())
                return status;

            status = file.read_exact_at(
                offset,
                static_cast<void*>(encoded.data()),
                encoded.size(),
                offset
            );

            if (!status.is_ok())
                return status;

            buff =
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
            std::byte* buf,
            std::uint32_t size,
            std::uint64_t& offset,
            const std::uint32_t BLOCK_SIZE
        )
        {
            if (size == 0)
                return Status::ok();

            if (buf == nullptr) {
                return Status{
                    StatusCode::InvalidArgument,
                    "read_bytes received a null buffer"
                };
            }

            Status status = ensure_fits_in_block(
                file,
                static_cast<std::size_t>(size),
                offset,
                BLOCK_SIZE
            );

            if (!status.is_ok())
                return status;

            status = file.read_exact_at(
                offset,
                static_cast<void*>(buf),
                static_cast<std::size_t>(size),
                offset
            );

            if (!status.is_ok())
                return status;

            return Status::ok();
        }

        Status read_bytes_with_u32_size(
            ReadableFile& file,
            std::span<std::byte> buf,
            std::uint32_t& bytes_read,
            std::uint64_t& offset,
            const std::uint32_t BLOCK_SIZE
        )
        {
            std::uint32_t encoded_size = 0;

            Status status = read_u32_t_le(
                file,
                encoded_size,
                offset,
                BLOCK_SIZE
            );

            if (!status.is_ok())
                return status;

            if (encoded_size > buf.size_bytes()) {
                return Status{
                    StatusCode::InvalidArgument,
                    "destination buffer is smaller than encoded byte sequence"
                };
            }

            status = read_bytes(
                file,
                buf.data(),
                encoded_size,
                offset,
                BLOCK_SIZE
            );

            if (!status.is_ok())
                return status;

            bytes_read = encoded_size;

            return Status::ok();
        }
    }
}