#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "sstable_entities/file_footer_section.h"

namespace
{
    using namespace SSTableEntities;

    constexpr std::uint64_t kDataOffset = BLOCK_SIZE;
    constexpr std::uint64_t kDataBlockCount = 2;
    constexpr std::uint64_t kIndexOffset = 3u * BLOCK_SIZE;
    constexpr std::uint32_t kIndexSize = 100;
    constexpr std::uint64_t kBloomOffset = 4u * BLOCK_SIZE;
    constexpr std::uint32_t kBloomSize = 153;
    constexpr std::uint64_t kMetaOffset = 5u * BLOCK_SIZE;
    constexpr std::uint32_t kMetaSize = 65;
    constexpr std::uint64_t kFooterOffset = 6u * BLOCK_SIZE;

    class MemoryWritableFile final : public WritableFile
    {
    public:
        explicit MemoryWritableFile(
            std::size_t initial_size = 0,
            std::uint64_t initial_position = 0,
            std::size_t fail_after_bytes =
            std::numeric_limits<std::size_t>::max()
        )
            : position_(initial_position),
            fail_after_bytes_(fail_after_bytes),
            bytes_(initial_size)
        {
        }

        Status append(
            const void* data,
            std::size_t size,
            std::uint64_t& tracked_offset
        ) override
        {
            if (tracked_offset != position_) {
                return Status{
                    StatusCode::InvalidOffset,
                    "tracked offset differs from memory-file position"
                };
            }

            const std::size_t remaining_before_failure =
                bytes_written_ >= fail_after_bytes_
                ? 0
                : fail_after_bytes_ - bytes_written_;
            const std::size_t writable =
                std::min(size, remaining_before_failure);

            if (writable > 0) {
                const std::uint64_t required64 = position_ + writable;
                if (required64 > std::numeric_limits<std::size_t>::max()) {
                    return Status{
                        StatusCode::AllocationTooLarge,
                        "memory-file size cannot be represented"
                    };
                }

                const std::size_t required =
                    static_cast<std::size_t>(required64);
                if (bytes_.size() < required) {
                    bytes_.resize(required);
                }

                std::memcpy(
                    bytes_.data() + static_cast<std::size_t>(position_),
                    data,
                    writable
                );

                position_ += writable;
                tracked_offset += writable;
                bytes_written_ += writable;
            }

            if (writable != size) {
                return Status{
                    StatusCode::WriteFailed,
                    "injected memory-file write failure"
                };
            }

            return Status::ok();
        }

        Status sync() override { return Status::ok(); }
        Status close() override { return Status::ok(); }

        Result<std::uint64_t> current_position() override
        {
            return Result<std::uint64_t>::ok(position_);
        }

        Status get_file_size(std::uint64_t& size_out) override
        {
            size_out = bytes_.size();
            return Status::ok();
        }

        Status durable_rename(
            const std::filesystem::path&,
            bool
        ) override
        {
            return Status::ok();
        }

        Status sync_parent_directory() override
        {
            return Status::ok();
        }

        std::filesystem::path parent_directory() override
        {
            return {};
        }

        Result<std::uint64_t> seek_to_end() override
        {
            position_ = bytes_.size();
            return Result<std::uint64_t>::ok(position_);
        }

#ifdef _WIN32
        const void* get_descriptor() const override
        {
            return nullptr;
        }
#else
        const int& get_descriptor() const override
        {
            return descriptor_;
        }
#endif

        [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept
        {
            return bytes_;
        }

    private:
        std::uint64_t position_ = 0;
        std::size_t bytes_written_ = 0;
        std::size_t fail_after_bytes_ =
            std::numeric_limits<std::size_t>::max();
        std::vector<std::byte> bytes_;
#ifndef _WIN32
        int descriptor_ = -1;
#endif
    };

    class MemoryReadableFile final : public ReadableFile
    {
    public:
        explicit MemoryReadableFile(std::vector<std::byte> bytes)
            : bytes_(std::move(bytes))
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
            if (offset > bytes_.size()) {
                return Status{
                    StatusCode::InvalidOffset,
                    "read offset exceeds memory-file size"
                };
            }

            const std::size_t available =
                bytes_.size() - static_cast<std::size_t>(offset);
            bytes_read = std::min(size, available);

            if (bytes_read > 0) {
                std::memcpy(
                    buffer,
                    bytes_.data() + static_cast<std::size_t>(offset),
                    bytes_read
                );
            }

            return Status::ok();
        }

        Status close() override { return Status::ok(); }

        Status get_file_size(std::uint64_t& size_out) override
        {
            size_out = bytes_.size();
            return Status::ok();
        }

#ifdef _WIN32
        const void* get_descriptor() const override
        {
            return nullptr;
        }
#else
        const int& get_descriptor() const override
        {
            return descriptor_;
        }
#endif

    private:
        std::vector<std::byte> bytes_;
#ifndef _WIN32
        int descriptor_ = -1;
#endif
    };

    [[nodiscard]] FileFooterSection make_valid_footer()
    {
        FileFooterSection footer;
        footer.data_offset = kDataOffset;
        footer.data_block_count = kDataBlockCount;
        footer.index_offset = kIndexOffset;
        footer.index_size = kIndexSize;
        footer.bloom_offset = kBloomOffset;
        footer.bloom_size = kBloomSize;
        footer.meta_offset = kMetaOffset;
        footer.meta_size = kMetaSize;
        footer.file_size = kFooterOffset + FileFooterSection::disk_size();
        footer.calculate_crc32(footer.footer_crc32);
        return footer;
    }

    void append_u32_le(
        std::vector<std::byte>& bytes,
        std::uint32_t value
    )
    {
        for (std::size_t i = 0; i < sizeof(value); ++i) {
            bytes.push_back(static_cast<std::byte>(
                (value >> (i * 8u)) & 0xFFu
                ));
        }
    }

    void append_u64_le(
        std::vector<std::byte>& bytes,
        std::uint64_t value
    )
    {
        for (std::size_t i = 0; i < sizeof(value); ++i) {
            bytes.push_back(static_cast<std::byte>(
                (value >> (i * 8u)) & 0xFFu
                ));
        }
    }

    [[nodiscard]] std::vector<std::byte> encode_footer(
        FileFooterSection footer,
        std::size_t trailing_bytes = 0
    )
    {
        footer.calculate_crc32(footer.footer_crc32);

        const std::uint64_t footer_offset =
            footer.file_size - FileFooterSection::disk_size();
        std::vector<std::byte> result(
            static_cast<std::size_t>(footer_offset),
            std::byte{ 0 }
        );

        append_u32_le(result, footer.magic);
        append_u32_le(result, footer.version);
        append_u32_le(result, footer.reserved);
        append_u64_le(result, footer.data_offset);
        append_u64_le(result, footer.data_block_count);
        append_u64_le(result, footer.index_offset);
        append_u32_le(result, footer.index_size);
        append_u64_le(result, footer.bloom_offset);
        append_u32_le(result, footer.bloom_size);
        append_u64_le(result, footer.meta_offset);
        append_u32_le(result, footer.meta_size);
        append_u64_le(result, footer.file_size);
        append_u32_le(result, footer.footer_crc32);
        result.resize(result.size() + trailing_bytes, std::byte{ 0 });
        return result;
    }

    [[nodiscard]] std::uint32_t read_u32_le(
        const std::vector<std::byte>& bytes,
        std::size_t offset
    )
    {
        std::uint32_t result = 0;
        for (std::size_t i = 0; i < sizeof(result); ++i) {
            result |= std::to_integer<std::uint32_t>(bytes.at(offset + i))
                << (i * 8u);
        }
        return result;
    }

    [[nodiscard]] std::uint64_t read_u64_le(
        const std::vector<std::byte>& bytes,
        std::size_t offset
    )
    {
        std::uint64_t result = 0;
        for (std::size_t i = 0; i < sizeof(result); ++i) {
            result |= std::to_integer<std::uint64_t>(bytes.at(offset + i))
                << (i * 8u);
        }
        return result;
    }

    void expect_same_footer(
        const FileFooterSection& actual,
        const FileFooterSection& expected
    )
    {
        EXPECT_EQ(actual.magic, expected.magic);
        EXPECT_EQ(actual.version, expected.version);
        EXPECT_EQ(actual.reserved, expected.reserved);
        EXPECT_EQ(actual.data_offset, expected.data_offset);
        EXPECT_EQ(actual.data_block_count, expected.data_block_count);
        EXPECT_EQ(actual.index_offset, expected.index_offset);
        EXPECT_EQ(actual.index_size, expected.index_size);
        EXPECT_EQ(actual.bloom_offset, expected.bloom_offset);
        EXPECT_EQ(actual.bloom_size, expected.bloom_size);
        EXPECT_EQ(actual.meta_offset, expected.meta_offset);
        EXPECT_EQ(actual.meta_size, expected.meta_size);
        EXPECT_EQ(actual.file_size, expected.file_size);
        EXPECT_EQ(actual.footer_crc32, expected.footer_crc32);
    }
}

TEST(FileFooterSectionLayoutTest, UsesStableVersionOneWidth)
{
    EXPECT_EQ(FileFooterSection::disk_size(), 76u);
    static_assert(std::is_copy_constructible_v<FileFooterSection>);
    static_assert(std::is_copy_assignable_v<FileFooterSection>);
    static_assert(std::is_nothrow_move_constructible_v<FileFooterSection>);
    static_assert(std::is_nothrow_move_assignable_v<FileFooterSection>);
}

TEST(FileFooterSectionTest, ConstructorInitializesCanonicalIdentity)
{
    FileFooterSection footer;
    EXPECT_EQ(footer.magic, FILE_FOOTER_MAGIC);
    EXPECT_EQ(footer.version, LATEST_SSTABLE_VERSION);
    EXPECT_EQ(footer.reserved, 0u);

    std::uint32_t crc = 0;
    footer.calculate_crc32(crc);
    EXPECT_EQ(footer.footer_crc32, crc);
}

TEST(FileFooterSectionTest, WriteFinalizesDerivedFieldsAndUsesLittleEndian)
{
    FileFooterSection footer = make_valid_footer();
    footer.magic = 0;
    footer.version = 999;
    footer.reserved = 123;
    footer.file_size = 0;
    footer.footer_crc32 = 0;

    MemoryWritableFile file(kFooterOffset, kFooterOffset);
    std::uint64_t offset = kFooterOffset;

    const Status status = footer.write(file, offset);
    ASSERT_TRUE(status.is_ok()) << status.message;

    EXPECT_EQ(offset, kFooterOffset + FileFooterSection::disk_size());
    EXPECT_EQ(footer.magic, FILE_FOOTER_MAGIC);
    EXPECT_EQ(footer.version, LATEST_SSTABLE_VERSION);
    EXPECT_EQ(footer.reserved, 0u);
    EXPECT_EQ(footer.file_size, offset);

    const std::size_t base = static_cast<std::size_t>(kFooterOffset);
    EXPECT_EQ(read_u32_le(file.bytes(), base + 0), FILE_FOOTER_MAGIC);
    EXPECT_EQ(read_u32_le(file.bytes(), base + 4), LATEST_SSTABLE_VERSION);
    EXPECT_EQ(read_u32_le(file.bytes(), base + 8), 0u);
    EXPECT_EQ(read_u64_le(file.bytes(), base + 12), kDataOffset);
    EXPECT_EQ(read_u64_le(file.bytes(), base + 20), kDataBlockCount);
    EXPECT_EQ(read_u64_le(file.bytes(), base + 28), kIndexOffset);
    EXPECT_EQ(read_u32_le(file.bytes(), base + 36), kIndexSize);
    EXPECT_EQ(read_u64_le(file.bytes(), base + 40), kBloomOffset);
    EXPECT_EQ(read_u32_le(file.bytes(), base + 48), kBloomSize);
    EXPECT_EQ(read_u64_le(file.bytes(), base + 52), kMetaOffset);
    EXPECT_EQ(read_u32_le(file.bytes(), base + 60), kMetaSize);
    EXPECT_EQ(read_u64_le(file.bytes(), base + 64), offset);
    EXPECT_EQ(read_u32_le(file.bytes(), base + 72), footer.footer_crc32);
}

TEST(FileFooterSectionTest, RejectsCursorMismatchBeforeWriting)
{
    FileFooterSection footer = make_valid_footer();
    const FileFooterSection before = footer;
    MemoryWritableFile file(kFooterOffset, kFooterOffset);
    std::uint64_t offset = kFooterOffset - 1;

    const Status status = footer.write(file, offset);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::InvalidOffset);
    EXPECT_EQ(file.bytes().size(), kFooterOffset);
    expect_same_footer(footer, before);
}

TEST(FileFooterSectionTest, RejectsUnalignedFooterBeforeWriting)
{
    FileFooterSection footer = make_valid_footer();
    MemoryWritableFile file(kFooterOffset + 1, kFooterOffset + 1);
    std::uint64_t offset = kFooterOffset + 1;

    const Status status = footer.write(file, offset);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::InvalidBlockAlignment);
    EXPECT_EQ(file.bytes().size(), kFooterOffset + 1);
}

TEST(FileFooterSectionTest, RejectsInvalidLayoutBeforeWriting)
{
    FileFooterSection footer = make_valid_footer();
    footer.index_size = static_cast<std::uint32_t>(
        kBloomOffset - kIndexOffset + 1
        );

    MemoryWritableFile file(kFooterOffset, kFooterOffset);
    std::uint64_t offset = kFooterOffset;

    const Status status = footer.write(file, offset);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::OffsetOverlap);
    EXPECT_EQ(offset, kFooterOffset);
    EXPECT_EQ(file.bytes().size(), kFooterOffset);
}

TEST(FileFooterSectionTest, FailedWriteDoesNotCommitStagedFields)
{
    FileFooterSection footer = make_valid_footer();
    footer.magic = 0;
    footer.version = 777;
    footer.reserved = 42;
    footer.file_size = 0;
    footer.footer_crc32 = 0;
    const FileFooterSection before = footer;

    MemoryWritableFile file(kFooterOffset, kFooterOffset, 10);
    std::uint64_t offset = kFooterOffset;

    const Status status = footer.write(file, offset);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::WriteFailed);
    EXPECT_GT(offset, kFooterOffset);
    expect_same_footer(footer, before);
}

TEST(FileFooterSectionTest, RoundTripsFromEndOfFile)
{
    const FileFooterSection expected = make_valid_footer();
    MemoryReadableFile file(encode_footer(expected));
    std::uint64_t offset = 123;

    Result<FileFooterSection> loaded = FileFooterSection::load(file, offset);

    ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;
    expect_same_footer(loaded.value, expected);
    EXPECT_EQ(offset, expected.file_size);
}

TEST(FileFooterSectionTest, RoundTripsFromExplicitFooterOffset)
{
    const FileFooterSection expected = make_valid_footer();
    MemoryReadableFile file(encode_footer(expected));
    std::uint64_t offset = kFooterOffset;

    Result<FileFooterSection> loaded =
        FileFooterSection::load(file, offset, 0);

    ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;
    expect_same_footer(loaded.value, expected);
    EXPECT_EQ(offset, expected.file_size);
}

TEST(FileFooterSectionTest, EveryTruncatedFooterFailsWithoutAdvancingOffset)
{
    for (std::size_t size = 0;
        size < FileFooterSection::disk_size();
        ++size) {
        MemoryReadableFile file{ std::vector<std::byte>(size) };
        std::uint64_t offset = 0;

        Result<FileFooterSection> loaded =
            FileFooterSection::load(file, offset, 0);

        ASSERT_FALSE(loaded.is_ok()) << "truncated size=" << size;
        EXPECT_EQ(offset, 0u) << "truncated size=" << size;
    }
}

TEST(FileFooterSectionTest, CorruptedVersionIsReportedAsChecksumMismatchFirst)
{
    FileFooterSection footer = make_valid_footer();
    std::vector<std::byte> bytes = encode_footer(footer);
    bytes.at(static_cast<std::size_t>(kFooterOffset + 4)) ^=
        std::byte{ 0x01 };

    MemoryReadableFile file(std::move(bytes));
    std::uint64_t offset = 777;
    Result<FileFooterSection> loaded = FileFooterSection::load(file, offset);

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::ChecksumMismatch);
    EXPECT_EQ(offset, 777u);
}

TEST(FileFooterSectionTest, ValidCrcWithUnsupportedVersionIsRejected)
{
    FileFooterSection footer = make_valid_footer();
    footer.version = LATEST_SSTABLE_VERSION + 1;
    MemoryReadableFile file(encode_footer(footer));
    std::uint64_t offset = 0;

    Result<FileFooterSection> loaded = FileFooterSection::load(file, offset);

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::UnsupportedVersion);
    EXPECT_EQ(offset, 0u);
}

TEST(FileFooterSectionTest, ValidCrcWithBadMagicIsRejected)
{
    FileFooterSection footer = make_valid_footer();
    footer.magic ^= 0x10u;
    MemoryReadableFile file(encode_footer(footer));
    std::uint64_t offset = 0;

    Result<FileFooterSection> loaded = FileFooterSection::load(file, offset);

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::BadMagic);
}

TEST(FileFooterSectionTest, ValidCrcWithNonzeroReservedFieldIsRejected)
{
    FileFooterSection footer = make_valid_footer();
    footer.reserved = 1;
    MemoryReadableFile file(encode_footer(footer));
    std::uint64_t offset = 0;

    Result<FileFooterSection> loaded = FileFooterSection::load(file, offset);

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::InvalidFooter);
}

TEST(FileFooterSectionTest, RejectsTrailingBytesAfterExplicitFooter)
{
    FileFooterSection footer = make_valid_footer();
    MemoryReadableFile file(encode_footer(footer, 1));
    std::uint64_t offset = kFooterOffset;

    Result<FileFooterSection> loaded =
        FileFooterSection::load(file, offset, 0);

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::InvalidFooter);
    EXPECT_EQ(offset, kFooterOffset);
}

TEST(FileFooterSectionTest, RejectsEncodedFileSizeMismatchWithValidCrc)
{
    FileFooterSection footer = make_valid_footer();
    --footer.file_size;

    // Preserve the physical footer position while encoding the corrupted
    // file_size field.
    const std::uint64_t encoded_size = footer.file_size;
    footer.file_size = kFooterOffset + FileFooterSection::disk_size() - 1;
    footer.calculate_crc32(footer.footer_crc32);

    std::vector<std::byte> bytes(static_cast<std::size_t>(kFooterOffset));
    append_u32_le(bytes, footer.magic);
    append_u32_le(bytes, footer.version);
    append_u32_le(bytes, footer.reserved);
    append_u64_le(bytes, footer.data_offset);
    append_u64_le(bytes, footer.data_block_count);
    append_u64_le(bytes, footer.index_offset);
    append_u32_le(bytes, footer.index_size);
    append_u64_le(bytes, footer.bloom_offset);
    append_u32_le(bytes, footer.bloom_size);
    append_u64_le(bytes, footer.meta_offset);
    append_u32_le(bytes, footer.meta_size);
    append_u64_le(bytes, footer.file_size);
    append_u32_le(bytes, footer.footer_crc32);
    ASSERT_EQ(encoded_size + 1, bytes.size());

    MemoryReadableFile file(std::move(bytes));
    std::uint64_t offset = 0;
    Result<FileFooterSection> loaded = FileFooterSection::load(file, offset);

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::InvalidFooter);
}

TEST(FileFooterSectionTest, RejectsMisalignedSectionOffsets)
{
    FileFooterSection footer = make_valid_footer();
    ++footer.bloom_offset;
    MemoryReadableFile file(encode_footer(footer));
    std::uint64_t offset = 0;

    Result<FileFooterSection> loaded = FileFooterSection::load(file, offset);

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::InvalidSectionOffset);
}

TEST(FileFooterSectionTest, RejectsDataBlockCountThatDoesNotReachIndex)
{
    FileFooterSection footer = make_valid_footer();
    footer.data_block_count = 1;
    MemoryReadableFile file(encode_footer(footer));
    std::uint64_t offset = 0;

    Result<FileFooterSection> loaded = FileFooterSection::load(file, offset);

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::OffsetOverlap);
}

TEST(FileFooterSectionTest, RejectsSectionLogicalSizePastNextSection)
{
    FileFooterSection footer = make_valid_footer();
    footer.index_size = static_cast<std::uint32_t>(
        footer.bloom_offset - footer.index_offset + 1
        );
    MemoryReadableFile file(encode_footer(footer));
    std::uint64_t offset = 0;

    Result<FileFooterSection> loaded = FileFooterSection::load(file, offset);

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::OffsetOverlap);
}

TEST(FileFooterSectionTest, RejectsWrongVersionOneBloomSize)
{
    FileFooterSection footer = make_valid_footer();
    --footer.bloom_size;
    MemoryReadableFile file(encode_footer(footer));
    std::uint64_t offset = 0;

    Result<FileFooterSection> loaded = FileFooterSection::load(file, offset);

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::InvalidSectionSize);
}

TEST(FileFooterSectionTest, RejectsMetadataSmallerThanFixedEncoding)
{
    FileFooterSection footer = make_valid_footer();
    footer.meta_size = 64;
    MemoryReadableFile file(encode_footer(footer));
    std::uint64_t offset = 0;

    Result<FileFooterSection> loaded = FileFooterSection::load(file, offset);

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::InvalidSectionSize);
}

TEST(FileFooterSectionTest, EmptyDataSectionMayKeepAnUnalignedLogicalCursor)
{
    FileFooterSection footer;
    footer.data_offset = 24;
    footer.data_block_count = 0;
    footer.index_offset = BLOCK_SIZE;
    footer.index_size = 9;
    footer.bloom_offset = 2u * BLOCK_SIZE;
    footer.bloom_size = kBloomSize;
    footer.meta_offset = 3u * BLOCK_SIZE;
    footer.meta_size = kMetaSize;
    const std::uint64_t footer_offset = 4u * BLOCK_SIZE;
    footer.file_size = footer_offset + FileFooterSection::disk_size();
    footer.calculate_crc32(footer.footer_crc32);

    const Status status = footer.validate(footer_offset, footer.file_size);
    EXPECT_TRUE(status.is_ok()) << status.message;
}

TEST(FileFooterSectionTest, WrongBackwardsOffsetDoesNotAdvanceCallerOffset)
{
    FileFooterSection footer = make_valid_footer();
    MemoryReadableFile file(encode_footer(footer));
    std::uint64_t offset = 333;

    Result<FileFooterSection> loaded = FileFooterSection::load(
        file,
        offset,
        FileFooterSection::disk_size() - 1
    );

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::InvalidBlockAlignment);
    EXPECT_EQ(offset, 333u);
}