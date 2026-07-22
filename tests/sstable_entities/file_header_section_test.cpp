#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "sstable_entities/file_header_section.h"

using SSTableEntities::BLOCK_SIZE;
using SSTableEntities::FILE_HEADER_MAGIC;
using SSTableEntities::FileHeaderSection;
using SSTableEntities::LATEST_SSTABLE_VERSION;

static_assert(std::is_copy_constructible_v<FileHeaderSection>);
static_assert(std::is_copy_assignable_v<FileHeaderSection>);
static_assert(std::is_move_constructible_v<FileHeaderSection>);
static_assert(std::is_move_assignable_v<FileHeaderSection>);

namespace
{
    class MemoryWritableFile final : public WritableFile
    {
    public:
        explicit MemoryWritableFile(std::vector<std::byte> initial = {})
            : bytes(std::move(initial)), cursor(bytes.size())
        {
        }

        Status append(
            const void* data,
            std::size_t size,
            std::uint64_t& track_offset
        ) override
        {
            if (track_offset != cursor) {
                return Status{
                    StatusCode::InvalidOffset,
                    "tracked offset differs from memory writer cursor"
                };
            }

            if (size != 0 && data == nullptr) {
                return Status{ StatusCode::NullPointer, "null append source" };
            }

            if (cursor > std::numeric_limits<std::size_t>::max() - size) {
                return Status{ StatusCode::DataTypeOverflow, "memory writer overflow" };
            }

            const auto new_size = static_cast<std::size_t>(cursor) + size;
            bytes.resize(new_size);
            if (size != 0) {
                std::memcpy(bytes.data() + cursor, data, size);
            }

            cursor += size;
            track_offset = cursor;
            return Status::ok();
        }

        Status sync() override { return Status::ok(); }
        Status close() override { return Status::ok(); }

        Result<std::uint64_t> current_position() override
        {
            return Result<std::uint64_t>::ok(cursor);
        }

        Status get_file_size(std::uint64_t& size_out) override
        {
            size_out = bytes.size();
            return Status::ok();
        }

        Status durable_rename(const std::filesystem::path&, bool) override
        {
            return Status::ok();
        }

        Status sync_parent_directory() override { return Status::ok(); }
        std::filesystem::path parent_directory() override { return {}; }

        Result<std::uint64_t> seek_to_end() override
        {
            cursor = bytes.size();
            return Result<std::uint64_t>::ok(cursor);
        }

#ifdef _WIN32
        const void* get_descriptor() const override { return nullptr; }
#else
        const int& get_descriptor() const override { return descriptor; }
#endif

        std::vector<std::byte> bytes;
        std::uint64_t cursor = 0;

    private:
#ifndef _WIN32
        int descriptor = -1;
#endif
    };

    class MemoryReadableFile final : public ReadableFile
    {
    public:
        explicit MemoryReadableFile(std::vector<std::byte> input)
            : bytes(std::move(input))
        {
        }

        Status read_at(
            std::uint64_t offset,
            void* buffer,
            std::size_t size,
            std::size_t& bytes_read
        ) override
        {
            bytes_read = 0;

            if (size != 0 && buffer == nullptr) {
                return Status{ StatusCode::NullPointer, "null read destination" };
            }

            if (offset >= bytes.size()) {
                return Status::ok();
            }

            const auto available = bytes.size() - static_cast<std::size_t>(offset);
            bytes_read = std::min(size, available);
            std::memcpy(buffer, bytes.data() + offset, bytes_read);
            return Status::ok();
        }

        Status close() override { return Status::ok(); }

        Status get_file_size(std::uint64_t& size_out) override
        {
            size_out = bytes.size();
            return Status::ok();
        }

#ifdef _WIN32
        const void* get_descriptor() const override { return nullptr; }
#else
        const int& get_descriptor() const override { return descriptor; }
#endif

        std::vector<std::byte> bytes;

    private:
#ifndef _WIN32
        int descriptor = -1;
#endif
    };

    void append_u32_le(std::vector<std::byte>& out, std::uint32_t value)
    {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
        }
    }

    std::vector<std::byte> encode_header(FileHeaderSection header)
    {
        EXPECT_TRUE(header.calculate_crc32(header.crc32).is_ok());

        std::vector<std::byte> bytes;
        bytes.reserve(FileHeaderSection::disk_size());
        append_u32_le(bytes, header.magic);
        append_u32_le(bytes, header.version);
        append_u32_le(bytes, header.flags);
        append_u32_le(bytes, header.block_size);
        append_u32_le(bytes, header.table_id);
        append_u32_le(bytes, header.crc32);
        return bytes;
    }
}

TEST(FileHeaderSectionTest, DiskSizeIsStable)
{
    EXPECT_EQ(FileHeaderSection::disk_size(), 24u);
}

TEST(FileHeaderSectionTest, ConstructorInitializesFieldsAndCanonicalCrc)
{
    FileHeaderSection header(0xA1B2C3D4u);

    EXPECT_EQ(header.magic, FILE_HEADER_MAGIC);
    EXPECT_EQ(header.version, LATEST_SSTABLE_VERSION);
    EXPECT_EQ(header.flags, 0u);
    EXPECT_EQ(header.block_size, BLOCK_SIZE);
    EXPECT_EQ(header.table_id, 0xA1B2C3D4u);

    std::uint32_t expected_crc = 0;
    ASSERT_TRUE(header.calculate_crc32(expected_crc).is_ok());
    EXPECT_EQ(header.crc32, expected_crc);
}

TEST(FileHeaderSectionTest, WriteUsesLittleEndianAndRefreshesStaleCrc)
{
    FileHeaderSection header(0x11223344u);
    header.flags = 0xAABBCCDDu;
    header.crc32 = 0; // write() must not trust cached CRC state.

    MemoryWritableFile file;
    std::uint64_t offset = 0;

    ASSERT_TRUE(header.write(file, offset).is_ok());
    EXPECT_EQ(offset, FileHeaderSection::disk_size());
    EXPECT_EQ(file.bytes, encode_header(header));

    EXPECT_EQ(file.bytes[0], std::byte{ 0x31 });
    EXPECT_EQ(file.bytes[1], std::byte{ 0x54 });
    EXPECT_EQ(file.bytes[2], std::byte{ 0x53 });
    EXPECT_EQ(file.bytes[3], std::byte{ 0x53 });
}

TEST(FileHeaderSectionTest, WriteAlignsToNextBlockWithoutFirstBlockAssertion)
{
    MemoryWritableFile file(std::vector<std::byte>(7, std::byte{ 0x11 }));
    std::uint64_t offset = 7;
    FileHeaderSection header(42);

    ASSERT_TRUE(header.write(file, offset).is_ok());
    EXPECT_EQ(offset, static_cast<std::uint64_t>(BLOCK_SIZE) + FileHeaderSection::disk_size());

    for (std::size_t i = 7; i < BLOCK_SIZE; ++i) {
        EXPECT_EQ(file.bytes[i], std::byte{ 0xCD });
    }
}

TEST(FileHeaderSectionTest, RoundTripsFromAnAlignedSection)
{
    MemoryWritableFile writable(std::vector<std::byte>(13, std::byte{ 0xEE }));
    std::uint64_t write_offset = 13;

    FileHeaderSection original(777);
    original.flags = 0x1234u;
    ASSERT_TRUE(original.write(writable, write_offset).is_ok());

    MemoryReadableFile readable(writable.bytes);
    std::uint64_t read_offset = 13;
    auto loaded = FileHeaderSection::load(readable, read_offset);

    ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;
    EXPECT_EQ(read_offset, write_offset);
    EXPECT_EQ(loaded.value.magic, original.magic);
    EXPECT_EQ(loaded.value.version, original.version);
    EXPECT_EQ(loaded.value.flags, original.flags);
    EXPECT_EQ(loaded.value.block_size, original.block_size);
    EXPECT_EQ(loaded.value.table_id, original.table_id);
    EXPECT_EQ(loaded.value.crc32, original.crc32);
}

TEST(FileHeaderSectionTest, LoadRejectsBadMagicAndRollsBackOffset)
{
    FileHeaderSection header(1);
    auto bytes = encode_header(header);
    bytes[0] ^= std::byte{ 0x01 };

    MemoryReadableFile file(std::move(bytes));
    std::uint64_t offset = 0;
    auto result = FileHeaderSection::load(file, offset);

    EXPECT_FALSE(result.is_ok());
    EXPECT_EQ(result.status.code, StatusCode::BadMagic);
    EXPECT_EQ(offset, 0u);
}

TEST(FileHeaderSectionTest, LoadChecksCrcBeforeInterpretingVersion)
{
    FileHeaderSection header(1);
    auto bytes = encode_header(header);

    // Corrupt the encoded version without recomputing CRC. This is corruption,
    // not a genuine request to read a newer file format.
    bytes[4] = std::byte{ 0x7F };

    MemoryReadableFile file(std::move(bytes));
    std::uint64_t offset = 0;
    auto result = FileHeaderSection::load(file, offset);

    EXPECT_FALSE(result.is_ok());
    EXPECT_EQ(result.status.code, StatusCode::ChecksumMismatch);
    EXPECT_EQ(offset, 0u);
}

TEST(FileHeaderSectionTest, LoadRejectsVersionZeroWithValidChecksum)
{
    FileHeaderSection header(1);
    header.version = 0;

    MemoryReadableFile file(encode_header(header));
    std::uint64_t offset = 0;
    auto result = FileHeaderSection::load(file, offset);

    EXPECT_FALSE(result.is_ok());
    EXPECT_EQ(result.status.code, StatusCode::UnsupportedVersion);
    EXPECT_EQ(offset, 0u);
}

TEST(FileHeaderSectionTest, LoadRejectsFutureVersionWithValidChecksum)
{
    FileHeaderSection header(1);
    header.version = LATEST_SSTABLE_VERSION + 1;

    MemoryReadableFile file(encode_header(header));
    std::uint64_t offset = 0;
    auto result = FileHeaderSection::load(file, offset);

    EXPECT_FALSE(result.is_ok());
    EXPECT_EQ(result.status.code, StatusCode::UnsupportedVersion);
    EXPECT_EQ(offset, 0u);
}

TEST(FileHeaderSectionTest, LoadRejectsUnsupportedBlockSizeWithValidChecksum)
{
    FileHeaderSection header(1);
    header.block_size = BLOCK_SIZE * 2;

    MemoryReadableFile file(encode_header(header));
    std::uint64_t offset = 0;
    auto result = FileHeaderSection::load(file, offset);

    EXPECT_FALSE(result.is_ok());
    EXPECT_EQ(result.status.code, StatusCode::UnsupportedBlockSize);
    EXPECT_EQ(offset, 0u);
}

TEST(FileHeaderSectionTest, LoadRejectsEveryTruncatedHeaderAndRollsBackOffset)
{
    const auto full = encode_header(FileHeaderSection(99));

    for (std::size_t size = 0; size < full.size(); ++size) {
        SCOPED_TRACE(size);
        MemoryReadableFile file(std::vector<std::byte>(full.begin(), full.begin() + size));
        std::uint64_t offset = 0;

        auto result = FileHeaderSection::load(file, offset);

        EXPECT_FALSE(result.is_ok());
        EXPECT_EQ(result.status.code, StatusCode::UnexpectedEOF);
        EXPECT_EQ(offset, 0u);
    }
}
 //Consider test above in case u want to implement truncated header recovery

TEST(FileHeaderSectionTest, WriteRejectsInvalidHeaderBeforeChangingFile)
{
    FileHeaderSection header(1);
    header.version = 0;

    MemoryWritableFile file;
    std::uint64_t offset = 0;
    const Status status = header.write(file, offset);

    EXPECT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::UnsupportedVersion);
    EXPECT_TRUE(file.bytes.empty());
    EXPECT_EQ(offset, 0u);
}