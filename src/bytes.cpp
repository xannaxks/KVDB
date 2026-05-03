#include "manifest.h"

#include <cstring>
#include <limits>

// If you already have your own CRC utilities, replace this include and crc32_of_bytes().
#include <zlib.h>

namespace
{
    enum class Tag : uint8_t
    {
        NextTableId = 1,
        LastSequenceNumber = 2,
        CurrentWalId = 3,
        AddTable = 4,
        DeleteTable = 5,
        End = 255
    };

    uint32_t crc32_of_bytes(const std::byte* data, std::size_t size)
    {
        uLong crc = ::crc32(0L, Z_NULL, 0);

        if (size > 0)
        {
            crc = ::crc32(
                crc,
                reinterpret_cast<const Bytef*>(data),
                static_cast<uInt>(size)
            );
        }

        return static_cast<uint32_t>(crc);
    }
    uint32_t crc32_of_vector(const std::vector<std::byte>& bytes)
    {
        return crc32_of_bytes(bytes.data(), bytes.size());
    }

    class ByteWriter
    {
    public:
        void put_u8(uint8_t value)
        {
            bytes_.push_back(static_cast<std::byte>(value));
        }

        void put_u32(uint32_t value)
        {
            for (int i = 0; i < 4; ++i)
            {
                bytes_.push_back(static_cast<std::byte>((value >> (i * 8)) & 0xFF));
            }
        }

        void put_u64(uint64_t value)
        {
            for (int i = 0; i < 8; ++i)
            {
                bytes_.push_back(static_cast<std::byte>((value >> (i * 8)) & 0xFF));
            }
        }

        void put_bytes(const std::vector<std::byte>& bytes)
        {
            put_u32(static_cast<uint32_t>(bytes.size()));
            bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
        }

        const std::vector<std::byte>& bytes() const
        {
            return bytes_;
        }

        std::vector<std::byte> take()
        {
            return std::move(bytes_);
        }

    private:
        std::vector<std::byte> bytes_;
    };

    class ByteReader
    {
    public:
        explicit ByteReader(const std::vector<std::byte>& bytes)
            : bytes_(bytes)
        {
        }

        bool read_u8(uint8_t& value)
        {
            if (remaining() < 1) return false;

            value = static_cast<uint8_t>(bytes_[pos_]);
            ++pos_;

            return true;
        }

        bool read_u32(uint32_t& value)
        {
            if (remaining() < 4) return false;

            value = 0;

            for (int i = 0; i < 4; ++i)
            {
                value |= static_cast<uint32_t>(bytes_[pos_ + i]) << (i * 8);
            }

            pos_ += 4;
            return true;
        }

        bool read_u64(uint64_t& value)
        {
            if (remaining() < 8) return false;

            value = 0;

            for (int i = 0; i < 8; ++i)
            {
                value |= static_cast<uint64_t>(bytes_[pos_ + i]) << (i * 8);
            }

            pos_ += 8;
            return true;
        }

        bool read_bytes(std::vector<std::byte>& output)
        {
            uint32_t size = 0;

            if (!read_u32(size)) return false;
            if (remaining() < size) return false;

            output.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(pos_),
                bytes_.begin() + static_cast<std::ptrdiff_t>(pos_ + size));

            pos_ += size;
            return true;
        }

        bool finished() const
        {
            return pos_ == bytes_.size();
        }

    private:
        std::size_t remaining() const
        {
            return bytes_.size() - pos_;
        }

    private:
        const std::vector<std::byte>& bytes_;
        std::size_t pos_ = 0;
    };

    void write_u32_to_file(std::ofstream& file, uint32_t value)
    {
        char bytes[4];

        for (int i = 0; i < 4; ++i)
        {
            bytes[i] = static_cast<char>((value >> (i * 8)) & 0xFF);
        }

        file.write(bytes, 4);
    }

    bool read_u32_from_file(std::ifstream& file, uint32_t& value)
    {
        char bytes[4];

        file.read(bytes, 4);

        if (file.gcount() == 0 && file.eof())
        {
            return false;
        }

        if (file.gcount() != 4)
        {
            return false;
        }

        value = 0;

        for (int i = 0; i < 4; ++i)
        {
            value |= static_cast<uint32_t>(
                static_cast<unsigned char>(bytes[i])
                ) << (i * 8);
        }

        return true;
    }

    std::vector<std::byte> encode_sstable_meta(const SSTableMeta& table)
    {
        ByteWriter writer;

        writer.put_u64(table.table_id);
        writer.put_u64(table.file_size);

        writer.put_u64(table.smallest_seq);
        writer.put_u64(table.largest_seq);

        writer.put_bytes(table.smallest_key);
        writer.put_bytes(table.largest_key);

        writer.put_u64(table.record_count);
        writer.put_u64(table.tombstone_count);

        writer.put_u32(table.level);

        return writer.take();
    }

    bool decode_sstable_meta(ByteReader& reader, SSTableMeta& table)
    {
        if (!reader.read_u64(table.table_id)) return false;
        if (!reader.read_u64(table.file_size)) return false;

        if (!reader.read_u64(table.smallest_seq)) return false;
        if (!reader.read_u64(table.largest_seq)) return false;

        if (!reader.read_bytes(table.smallest_key)) return false;
        if (!reader.read_bytes(table.largest_key)) return false;

        if (!reader.read_u64(table.record_count)) return false;
        if (!reader.read_u64(table.tombstone_count)) return false;

        if (!reader.read_u32(table.level)) return false;

        return true;
    }

    std::vector<std::byte> encode_version_edit_payload(const VersionEdit& edit)
    {
        ByteWriter writer;

        if (edit.next_table_id.has_value())
        {
            writer.put_u8(static_cast<uint8_t>(Tag::NextTableId));
            writer.put_u64(*edit.next_table_id);
        }

        if (edit.last_sequence_number.has_value())
        {
            writer.put_u8(static_cast<uint8_t>(Tag::LastSequenceNumber));
            writer.put_u64(*edit.last_sequence_number);
        }

        if (edit.current_wal_id.has_value())
        {
            writer.put_u8(static_cast<uint8_t>(Tag::CurrentWalId));
            writer.put_u64(*edit.current_wal_id);
        }

        for (const SSTableMeta& table : edit.added_tables)
        {
            writer.put_u8(static_cast<uint8_t>(Tag::AddTable));

            std::vector<std::byte> encoded_table = encode_sstable_meta(table);

            const auto& bytes = encoded_table;
            writer.put_u32(static_cast<uint32_t>(bytes.size()));

            for (std::byte byte : bytes)
            {
                writer.put_u8(static_cast<uint8_t>(byte));
            }
        }

        for (const DeletedTable& table : edit.deleted_tables)
        {
            writer.put_u8(static_cast<uint8_t>(Tag::DeleteTable));
            writer.put_u32(table.level);
            writer.put_u64(table.table_id);
        }

        writer.put_u8(static_cast<uint8_t>(Tag::End));

        return writer.take();
    }

    std::optional<VersionEdit> decode_version_edit_payload(const std::vector<std::byte>& payload)
    {
        ByteReader reader(payload);
        VersionEdit edit;

        while (true)
        {
            uint8_t raw_tag = 0;

            if (!reader.read_u8(raw_tag))
            {
                return std::nullopt;
            }

            Tag tag = static_cast<Tag>(raw_tag);

            switch (tag)
            {
            case Tag::NextTableId:
            {
                uint64_t value = 0;
                if (!reader.read_u64(value)) return std::nullopt;
                edit.next_table_id = value;
                break;
            }

            case Tag::LastSequenceNumber:
            {
                uint64_t value = 0;
                if (!reader.read_u64(value)) return std::nullopt;
                edit.last_sequence_number = value;
                break;
            }

            case Tag::CurrentWalId:
            {
                uint64_t value = 0;
                if (!reader.read_u64(value)) return std::nullopt;
                edit.current_wal_id = value;
                break;
            }

            case Tag::AddTable:
            {
                std::vector<std::byte> table_payload;

                if (!reader.read_bytes(table_payload))
                {
                    return std::nullopt;
                }

                ByteReader table_reader(table_payload);
                SSTableMeta table;

                if (!decode_sstable_meta(table_reader, table))
                {
                    return std::nullopt;
                }

                if (!table_reader.finished())
                {
                    return std::nullopt;
                }

                edit.added_tables.push_back(std::move(table));
                break;
            }

            case Tag::DeleteTable:
            {
                DeletedTable deleted;

                if (!reader.read_u32(deleted.level)) return std::nullopt;
                if (!reader.read_u64(deleted.table_id)) return std::nullopt;

                edit.deleted_tables.push_back(deleted);
                break;
            }

            case Tag::End:
            {
                if (!reader.finished())
                {
                    return std::nullopt;
                }

                return edit;
            }

            default:
            {
                return std::nullopt;
            }
            }
        }
    }
}