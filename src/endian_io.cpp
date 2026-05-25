#include "endian_io.h"

namespace kvdb
{
    namespace endian {
        void put_u8(std::vector<std::byte>& out, std::uint8_t value)
        {
            out.push_back(static_cast<std::byte>(value));
        }
        void put_u16_le(std::vector<std::byte>& out, std::uint16_t value)
        {
            out.push_back(static_cast<std::byte>((value >> 0) & 0xFF));
            out.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
        }
        void put_u32_le(std::vector<std::byte>& out, std::uint32_t value)
        {
            for (int shift = 0; shift < 32; shift += 8)
            {
                out.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
            }
        }
        void put_u64_le(std::vector<std::byte>& out, std::uint64_t value)
        {
            for (int shift = 0; shift < 64; shift += 8)
            {
                out.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
            }
        }
        void put_bytes_with_u32_size(
            std::vector<std::byte>& out,
            std::span<const std::byte> bytes
        )
        {
            // For your KVDB keys/values, u32 size is probably enough.
            // If you want values > 4GB later, switch to u64.
            put_u32_le(out, static_cast<std::uint32_t>(bytes.size()));
            out.insert(out.end(), bytes.begin(), bytes.end());
        }


        Status write_bytes(
            WritableFile& file,
            std::span<const std::byte> bytes
        )
        {
            if (!bytes.empty())
            {
                file.write(
                    reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size())
                );
            }

            return file.good();
        }
        Status write_u8(WritableFile& file, std::uint8_t value)
        {
            char byte = static_cast<char>(value);
            file.write(&byte, 1);
            return file.good();
        }
        Status write_u16_le(WritableFile& file, std::uint16_t value)
        {
            std::array<std::byte, 2> bytes{
                static_cast<std::byte>((value >> 0) & 0xFF),
                static_cast<std::byte>((value >> 8) & 0xFF)
            };

            file.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size())
            );

            return file.good();
        }
        Status write_u32_le(WritableFile& file, std::uint32_t value)
        {
            std::array<std::byte, 4> bytes{
                static_cast<std::byte>((value >> 0) & 0xFF),
                static_cast<std::byte>((value >> 8) & 0xFF),
                static_cast<std::byte>((value >> 16) & 0xFF),
                static_cast<std::byte>((value >> 24) & 0xFF)
            };

            file.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size())
            );

            return file.good();
        }
        Status write_u64_le(WritableFile& file, std::uint64_t value)
        {
            std::array<std::byte, 8> bytes{
                static_cast<std::byte>((value >> 0) & 0xFF),
                static_cast<std::byte>((value >> 8) & 0xFF),
                static_cast<std::byte>((value >> 16) & 0xFF),
                static_cast<std::byte>((value >> 24) & 0xFF),
                static_cast<std::byte>((value >> 32) & 0xFF),
                static_cast<std::byte>((value >> 40) & 0xFF),
                static_cast<std::byte>((value >> 48) & 0xFF),
                static_cast<std::byte>((value >> 56) & 0xFF)
            };

            file.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size())
            );

            return file.good();
        }
        Status write_bytes_with_u32_size(
            WritableFile& file,
            std::span<const std::byte> bytes
        )
        {
            if (bytes.size() > std::numeric_limits<std::uint32_t>::max())
            {
                return false;
            }

            if (!write_u32_le(file, static_cast<std::uint32_t>(bytes.size())))
            {
                return false;
            }

            return write_bytes(file, bytes);
        }

        std::optional<std::uint8_t> read_u8(ReadableFile& file)
        {
            char byte = 0;
            file.read(&byte, 1);

            if (file.gcount() != 1)
            {
                return std::nullopt;
            }

            return static_cast<std::uint8_t>(static_cast<unsigned char>(byte));
        }
        std::optional<std::uint16_t> read_u16_le(ReadableFile& file)
        {
            std::array<unsigned char, 2> bytes{};

            file.read(reinterpret_cast<char*>(bytes.data()), 2);

            if (file.gcount() != 2)
            {
                return std::nullopt;
            }

            std::uint16_t value = 0;

            value |= static_cast<std::uint16_t>(bytes[0]) << 0;
            value |= static_cast<std::uint16_t>(bytes[1]) << 8;

            return value;
        }
        std::optional<std::uint32_t> read_u32_le(ReadableFile& file)
        {
            std::array<unsigned char, 4> bytes{};

            file.read(reinterpret_cast<char*>(bytes.data()), 4);

            if (file.gcount() != 4)
            {
                return std::nullopt;
            }

            std::uint32_t value = 0;

            value |= static_cast<std::uint32_t>(bytes[0]) << 0;
            value |= static_cast<std::uint32_t>(bytes[1]) << 8;
            value |= static_cast<std::uint32_t>(bytes[2]) << 16;
            value |= static_cast<std::uint32_t>(bytes[3]) << 24;

            return value;
        }
        std::optional<std::uint64_t> read_u64_le(ReadableFile& file)
        {
            std::array<unsigned char, 8> bytes{};

            file.read(reinterpret_cast<char*>(bytes.data()), 8);

            if (file.gcount() != 8)
            {
                return std::nullopt;
            }

            std::uint64_t value = 0;

            value |= static_cast<std::uint64_t>(bytes[0]) << 0;
            value |= static_cast<std::uint64_t>(bytes[1]) << 8;
            value |= static_cast<std::uint64_t>(bytes[2]) << 16;
            value |= static_cast<std::uint64_t>(bytes[3]) << 24;
            value |= static_cast<std::uint64_t>(bytes[4]) << 32;
            value |= static_cast<std::uint64_t>(bytes[5]) << 40;
            value |= static_cast<std::uint64_t>(bytes[6]) << 48;
            value |= static_cast<std::uint64_t>(bytes[7]) << 56;

            return value;
        }
        std::optional<std::vector<std::byte>> read_bytes(
            ReadableFile& file,
            std::uint32_t size
        )
        {
            std::vector<std::byte> bytes(size);

            if (size == 0)
            {
                return bytes;
            }

            file.read(
                reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(size)
            );

            if (static_cast<std::uint32_t>(file.gcount()) != size)
            {
                return std::nullopt;
            }

            return bytes;
        }
        std::optional<std::vector<std::byte>> read_bytes_with_u32_size(
            ReadableFile& file
        )
        {
            auto size = read_u32_le(file);

            if (!size.has_value())
            {
                return std::nullopt;
            }

            return read_bytes(file, *size);
        }


        Reader::Reader(std::span<const std::byte> data)
            : data_(data)
        {
        }

        std::optional<std::uint8_t> Reader::read_u8()
        {
            if (remaining() < 1)
            {
                return std::nullopt;
            }

            std::uint8_t value = static_cast<std::uint8_t>(data_[pos_]);
            ++pos_;

            return value;
        }
        std::optional<std::uint16_t> Reader::read_u16_le()
        {
            if (remaining() < 2)
            {
                return std::nullopt;
            }

            std::uint16_t value = 0;

            value |= static_cast<std::uint16_t>(data_[pos_ + 0]) << 0;
            value |= static_cast<std::uint16_t>(data_[pos_ + 1]) << 8;

            pos_ += 2;

            return value;
        }
        std::optional<std::uint32_t> Reader::read_u32_le()
        {
            if (remaining() < 4)
            {
                return std::nullopt;
            }

            std::uint32_t value = 0;

            value |= static_cast<std::uint32_t>(data_[pos_ + 0]) << 0;
            value |= static_cast<std::uint32_t>(data_[pos_ + 1]) << 8;
            value |= static_cast<std::uint32_t>(data_[pos_ + 2]) << 16;
            value |= static_cast<std::uint32_t>(data_[pos_ + 3]) << 24;

            pos_ += 4;

            return value;
        }
        std::optional<std::uint64_t> Reader::read_u64_le()
        {
            if (remaining() < 8)
            {
                return std::nullopt;
            }

            std::uint64_t value = 0;

            value |= static_cast<std::uint64_t>(data_[pos_ + 0]) << 0;
            value |= static_cast<std::uint64_t>(data_[pos_ + 1]) << 8;
            value |= static_cast<std::uint64_t>(data_[pos_ + 2]) << 16;
            value |= static_cast<std::uint64_t>(data_[pos_ + 3]) << 24;
            value |= static_cast<std::uint64_t>(data_[pos_ + 4]) << 32;
            value |= static_cast<std::uint64_t>(data_[pos_ + 5]) << 40;
            value |= static_cast<std::uint64_t>(data_[pos_ + 6]) << 48;
            value |= static_cast<std::uint64_t>(data_[pos_ + 7]) << 56;

            pos_ += 8;

            return value;
        }
        std::optional<std::span<const std::byte>> Reader::read_bytes(std::uint32_t size)
        {
            if (remaining() < size)
            {
                return std::nullopt;
            }

            auto result = data_.subspan(pos_, size);
            pos_ += size;

            return result;
        }
        std::optional<std::vector<std::byte>> Reader::read_bytes_with_u32_size()
        {
            auto size = read_u32_le();

            if (!size.has_value())
            {
                return std::nullopt;
            }

            auto bytes = read_bytes(*size);

            if (!bytes.has_value())
            {
                return std::nullopt;
            }

            return std::vector<std::byte>(bytes->begin(), bytes->end());
        }

        bool Reader::finished() const
        {
            return pos_ == data_.size();
        }
        std::size_t Reader::remaining() const
        {
            return data_.size() - pos_;
        }
        std::size_t Reader::position() const
        {
            return pos_;
        }
    }
    namespace blockio
    {
    }
}