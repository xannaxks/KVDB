#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "crc32_helpers.h"
#include "sstable_entities/bloom_section.h"
#include "sstable_entities/data_section.h"

namespace
{
    using namespace SSTableEntities;

    class MemoryWritableFile final : public WritableFile
    {
    public:
        explicit MemoryWritableFile(
            std::uint64_t initial_position = 0,
            std::size_t fail_after_bytes =
            std::numeric_limits<std::size_t>::max()
        )
            : position_(initial_position),
            fail_after_bytes_(fail_after_bytes),
            bytes_(static_cast<std::size_t>(initial_position))
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
                    "test writable cursor mismatch"
                };
            }

            if (size > 0 && data == nullptr) {
                return Status{ StatusCode::NullPointer, "null append buffer" };
            }

            if (bytes_written_ > fail_after_bytes_ ||
                size > fail_after_bytes_ - bytes_written_) {
                return Status{ StatusCode::WriteFailed, "injected write failure" };
            }

            const std::size_t required =
                static_cast<std::size_t>(position_) + size;
            if (bytes_.size() < required) {
                bytes_.resize(required);
            }

            if (size > 0) {
                std::memcpy(
                    bytes_.data() + static_cast<std::size_t>(position_),
                    data,
                    size
                );
            }

            position_ += size;
            tracked_offset += size;
            bytes_written_ += size;
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

        Status sync_parent_directory() override { return Status::ok(); }
        std::filesystem::path parent_directory() override { return {}; }

        Result<std::uint64_t> seek_to_end() override
        {
            position_ = bytes_.size();
            return Result<std::uint64_t>::ok(position_);
        }

#ifdef _WIN32
        const void* get_descriptor() const override { return nullptr; }
#else
        const int& get_descriptor() const override
        {
            static const int descriptor = -1;
            return descriptor;
        }
#endif

        [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept
        {
            return bytes_;
        }

    private:
        std::uint64_t position_ = 0;
        std::size_t bytes_written_ = 0;
        std::size_t fail_after_bytes_;
        std::vector<std::byte> bytes_;
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
            if (size > 0 && buffer == nullptr) {
                return Status{ StatusCode::NullPointer, "null read buffer" };
            }

            if (offset >= bytes_.size()) {
                return Status::ok();
            }

            const std::size_t available =
                bytes_.size() - static_cast<std::size_t>(offset);
            bytes_read = std::min(size, available);
            std::memcpy(
                buffer,
                bytes_.data() + static_cast<std::size_t>(offset),
                bytes_read
            );
            return Status::ok();
        }

        Status close() override { return Status::ok(); }

        Status get_file_size(std::uint64_t& size_out) override
        {
            size_out = bytes_.size();
            return Status::ok();
        }

#ifdef _WIN32
        const void* get_descriptor() const override { return nullptr; }
#else
        const int& get_descriptor() const override
        {
            static const int descriptor = -1;
            return descriptor;
        }
#endif

    private:
        std::vector<std::byte> bytes_;
    };

    [[nodiscard]] std::uint32_t read_u32_le(
        const std::vector<std::byte>& bytes,
        std::size_t offset
    )
    {
        return std::to_integer<std::uint32_t>(bytes.at(offset)) |
            (std::to_integer<std::uint32_t>(bytes.at(offset + 1)) << 8u) |
            (std::to_integer<std::uint32_t>(bytes.at(offset + 2)) << 16u) |
            (std::to_integer<std::uint32_t>(bytes.at(offset + 3)) << 24u);
    }

    [[nodiscard]] std::uint64_t read_u64_le(
        const std::vector<std::byte>& bytes,
        std::size_t offset
    )
    {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i) {
            value |= static_cast<std::uint64_t>(
                std::to_integer<std::uint8_t>(bytes.at(offset + i))
                ) << (i * 8u);
        }
        return value;
    }

    void write_u32_le(
        std::vector<std::byte>& bytes,
        std::size_t offset,
        std::uint32_t value
    )
    {
        for (unsigned i = 0; i < 4; ++i) {
            bytes.at(offset + i) = static_cast<std::byte>(
                (value >> (i * 8u)) & 0xFFu
                );
        }
    }

    void refresh_serialized_payload_crc(
        std::vector<std::byte>& bytes,
        std::size_t section_offset = 0
    )
    {
        const std::size_t payload_offset =
            section_offset + BloomSection::Header::disk_size();
        const std::uint32_t crc = crc32_of(
            bytes.data() + payload_offset,
            BloomSection::Payload::disk_size()
        );
        write_u32_le(bytes, section_offset + 5, crc);
    }

    struct StoredKey
    {
        std::string bytes;
        DataSection::Payload payload;

        explicit StoredKey(std::string value)
            : bytes(std::move(value))
        {
            payload.key_ptr = bytes.empty() ? nullptr : bytes.data();
            payload.key_size = static_cast<std::uint32_t>(bytes.size());
        }
    };

    [[nodiscard]] std::vector<std::byte> serialize(
        BloomSection& bloom,
        std::uint64_t initial_offset = 0
    )
    {
        MemoryWritableFile file(initial_offset);
        std::uint64_t offset = initial_offset;
        std::uint64_t bloom_offset = std::numeric_limits<std::uint64_t>::max();
        const Status status = bloom.write(file, offset, bloom_offset);
        EXPECT_TRUE(status.is_ok()) << status.message;
        return file.bytes();
    }
}

TEST(BloomSectionLayoutTest, UsesStableSerializedWidths)
{
    EXPECT_EQ(BloomSection::Header::disk_size(), 9u);
    EXPECT_EQ(BloomSection::Payload::fixed_part_disk_size(), 16u);
    EXPECT_EQ(
        BloomSection::Payload::disk_size(),
        16u + BLOOM_MASK_BIT_SIZE
    );
    EXPECT_EQ(
        BloomSection::disk_size(),
        25u + BLOOM_MASK_BIT_SIZE
    );
}

TEST(BloomSectionTest, ConstructorCreatesValidEmptyFilter)
{
    BloomSection bloom;

    ASSERT_TRUE(bloom.validate().is_ok());
    EXPECT_EQ(bloom.header.type, BlockType::Bloom);
    EXPECT_EQ(bloom.header.payload_size, BloomSection::Payload::disk_size());
    EXPECT_EQ(bloom.payload.bloom_bits, BLOOM_MASK_BIT_SIZE);
    EXPECT_EQ(bloom.payload.hash_count, BLOOM_HASH_COUNT);
    EXPECT_EQ(bloom.payload.key_count, 0u);
    EXPECT_FALSE(bloom.may_contain("missing", 7));
}

TEST(BloomSectionTest, AddedKeysNeverProduceFalseNegatives)
{
    BloomSection bloom;
    std::vector<std::string> keys;

    for (int i = 0; i < 200; ++i) {
        keys.push_back("key-" + std::to_string(i));
        ASSERT_TRUE(
            bloom.add_key(keys.back().data(), keys.back().size()).is_ok()
        );
    }

    ASSERT_EQ(bloom.payload.key_count, keys.size());
    for (const std::string& key : keys) {
        EXPECT_TRUE(bloom.may_contain(key.data(), key.size())) << key;
    }
}

TEST(BloomSectionTest, SupportsTheEmptyKey)
{
    BloomSection bloom;
    ASSERT_TRUE(bloom.add_key(nullptr, 0).is_ok());
    EXPECT_TRUE(bloom.may_contain(nullptr, 0));
}

TEST(BloomSectionTest, RejectsNullNonEmptyKeyWithoutMutation)
{
    BloomSection bloom;
    const BloomSection before = bloom;

    const Status status = bloom.add_key(nullptr, 1);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::NullPointer);
    EXPECT_EQ(bloom.payload.key_count, before.payload.key_count);
    EXPECT_EQ(bloom.payload.mask, before.payload.mask);
}

TEST(BloomSectionTest, RejectsKeyCountOverflow)
{
    BloomSection bloom;
    bloom.payload.key_count = std::numeric_limits<std::uint32_t>::max();
    bloom.payload.mask[0] = 1;

    const Status status = bloom.add_key("x", 1);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::DataTypeOverflow);
}

TEST(BloomSectionTest, InvalidFilterFailsOpenInsteadOfHidingKeys)
{
    BloomSection bloom;
    bloom.payload.bloom_bits = 0;

    EXPECT_TRUE(bloom.may_contain("real-key", 8));
    EXPECT_TRUE(bloom.may_contain(nullptr, 1));
}

TEST(BloomSectionTest, RebuildRepairsMalformedPriorState)
{
    StoredKey first("alpha");
    StoredKey second("beta");

    DataSection data;
    data.data_blocks.emplace_back();
    data.data_blocks.back().payloads.push_back(first.payload);
    data.data_blocks.back().payloads.push_back(second.payload);

    BloomSection bloom;
    bloom.payload.mask.fill(1u);
    bloom.payload.bloom_bits = 0;

    const Status status = bloom.rebuild(data);

    ASSERT_TRUE(status.is_ok()) << status.message;
    ASSERT_TRUE(bloom.validate().is_ok());
    EXPECT_EQ(bloom.payload.mask.size(), BLOOM_MASK_BIT_SIZE);
    EXPECT_EQ(bloom.payload.key_count, 2u);
    EXPECT_TRUE(bloom.may_contain(first.bytes.data(), first.bytes.size()));
    EXPECT_TRUE(bloom.may_contain(second.bytes.data(), second.bytes.size()));
}

TEST(BloomSectionTest, FailedRebuildPreservesExistingFilter)
{
    BloomSection bloom;
    ASSERT_TRUE(bloom.add_key("existing", 8).is_ok());
    ASSERT_TRUE(bloom.recompute_crc32().is_ok());
    const BloomSection before = bloom;

    DataSection data;
    data.data_blocks.emplace_back();
    DataSection::Payload invalid;
    invalid.key_ptr = nullptr;
    invalid.key_size = 1;
    data.data_blocks.back().payloads.push_back(invalid);

    const Status status = bloom.rebuild(data);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::NullPointer);
    EXPECT_EQ(bloom.header.crc32, before.header.crc32);
    EXPECT_EQ(bloom.payload.key_count, before.payload.key_count);
    EXPECT_EQ(bloom.payload.mask, before.payload.mask);
}

TEST(BloomSectionTest, WriteRebuildsStaleDerivedHeader)
{
    BloomSection bloom;
    ASSERT_TRUE(bloom.add_key("alpha", 5).is_ok());
    bloom.header.type = BlockType::Index;
    bloom.header.payload_size = 1;
    bloom.header.crc32 = 2;

    MemoryWritableFile file;
    std::uint64_t offset = 0;
    std::uint64_t bloom_offset = 999;

    const Status status = bloom.write(file, offset, bloom_offset);

    ASSERT_TRUE(status.is_ok()) << status.message;
    ASSERT_TRUE(bloom.validate().is_ok());
    EXPECT_EQ(bloom_offset, 0u);
    EXPECT_EQ(offset, BloomSection::disk_size());

    const auto& bytes = file.bytes();
    ASSERT_EQ(bytes.size(), BloomSection::disk_size());
    EXPECT_EQ(
        std::to_integer<std::uint8_t>(bytes[0]),
        static_cast<std::uint8_t>(BlockType::Bloom)
    );
    EXPECT_EQ(read_u32_le(bytes, 1), BloomSection::Payload::disk_size());
    EXPECT_EQ(read_u32_le(bytes, 5), bloom.header.crc32);
    EXPECT_EQ(read_u64_le(bytes, 9), BLOOM_MASK_BIT_SIZE);
    EXPECT_EQ(read_u32_le(bytes, 17), BLOOM_HASH_COUNT);
    EXPECT_EQ(read_u32_le(bytes, 21), 1u);

    const std::uint32_t independent_crc = crc32_of(
        bytes.data() + BloomSection::Header::disk_size(),
        BloomSection::Payload::disk_size()
    );
    EXPECT_EQ(independent_crc, bloom.header.crc32);
}

TEST(BloomSectionTest, WriteAlignsAndCommitsBloomOffsetAfterSuccess)
{
    BloomSection bloom;
    MemoryWritableFile file(24);
    std::uint64_t offset = 24;
    std::uint64_t bloom_offset = 999;

    const Status status = bloom.write(file, offset, bloom_offset);

    ASSERT_TRUE(status.is_ok()) << status.message;
    EXPECT_EQ(bloom_offset, BLOCK_SIZE);
    EXPECT_EQ(offset, BLOCK_SIZE + BloomSection::disk_size());
}

TEST(BloomSectionTest, InvalidPayloadIsRejectedBeforeAlignmentOrMetadataCommit)
{
    BloomSection bloom;
    bloom.payload.bloom_bits = 0;

    MemoryWritableFile file(24);
    std::uint64_t offset = 24;
    std::uint64_t bloom_offset = 999;

    const Status status = bloom.write(file, offset, bloom_offset);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::InvalidPayloadSize);
    EXPECT_EQ(offset, 24u);
    EXPECT_EQ(bloom_offset, 999u);
    EXPECT_EQ(file.bytes().size(), 24u);
}

TEST(BloomSectionTest, CursorMismatchIsRuntimeError)
{
    BloomSection bloom;
    MemoryWritableFile file(10);
    std::uint64_t offset = 0;
    std::uint64_t bloom_offset = 999;

    const Status status = bloom.write(file, offset, bloom_offset);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::InvalidOffset);
    EXPECT_EQ(offset, 0u);
    EXPECT_EQ(bloom_offset, 999u);
}

TEST(BloomSectionTest, FailedPhysicalWritePreservesBloomOffset)
{
    BloomSection bloom;
    ASSERT_TRUE(bloom.add_key("alpha", 5).is_ok());

    MemoryWritableFile file(0, 17);
    std::uint64_t offset = 0;
    std::uint64_t bloom_offset = 777;

    const Status status = bloom.write(file, offset, bloom_offset);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::WriteFailed);
    EXPECT_EQ(bloom_offset, 777u);
    EXPECT_GT(offset, 0u);
}

TEST(BloomSectionPayloadTest, DoesNotSilentlyMoveAcrossBlockBoundary)
{
    BloomSection bloom;
    constexpr std::uint64_t start = BLOCK_SIZE - 10;
    MemoryWritableFile file(start);
    std::uint64_t offset = start;

    const Status status = bloom.payload.write(file, offset);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::SizeExceedsBlockBoundary);
    EXPECT_EQ(offset, start);
}

TEST(BloomSectionTest, RoundTripsThroughDiskFormat)
{
    BloomSection bloom;
    ASSERT_TRUE(bloom.add_key("alpha", 5).is_ok());
    ASSERT_TRUE(bloom.add_key("beta", 4).is_ok());

    MemoryWritableFile writable(24);
    std::uint64_t write_offset = 24;
    std::uint64_t bloom_offset = 0;
    ASSERT_TRUE(
        bloom.write(writable, write_offset, bloom_offset).is_ok()
    );

    MemoryReadableFile readable(writable.bytes());
    std::uint64_t read_offset = 777;
    Result<BloomSection> loaded = BloomSection::load(
        readable,
        read_offset,
        bloom_offset
    );

    ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;
    EXPECT_EQ(
        read_offset,
        bloom_offset + BloomSection::disk_size()
    );
    EXPECT_EQ(loaded.value.payload.key_count, 2u);
    EXPECT_EQ(loaded.value.payload.mask, bloom.payload.mask);
    EXPECT_TRUE(loaded.value.may_contain("alpha", 5));
    EXPECT_TRUE(loaded.value.may_contain("beta", 4));
}

TEST(BloomSectionLoadTest, RejectsMisalignedOffsetWithoutChangingCursor)
{
    BloomSection bloom;
    std::vector<std::byte> bytes = serialize(bloom);
    MemoryReadableFile readable(std::move(bytes));
    std::uint64_t offset = 321;

    Result<BloomSection> result = BloomSection::load(readable, offset, 1);

    ASSERT_FALSE(result.is_ok());
    EXPECT_EQ(result.status.code, StatusCode::InvalidBlockAlignment);
    EXPECT_EQ(offset, 321u);
}

TEST(BloomSectionLoadTest, RejectsEveryTruncatedLengthTransactionally)
{
    BloomSection bloom;
    ASSERT_TRUE(bloom.add_key("alpha", 5).is_ok());
    const std::vector<std::byte> complete = serialize(bloom);

    for (std::size_t length = 0; length < BloomSection::disk_size(); ++length) {
        std::vector<std::byte> truncated(complete.begin(), complete.begin() + length);
        MemoryReadableFile readable(std::move(truncated));
        std::uint64_t offset = 777;

        Result<BloomSection> result = BloomSection::load(readable, offset, 0);

        EXPECT_FALSE(result.is_ok()) << "length=" << length;
        EXPECT_EQ(offset, 777u) << "length=" << length;
    }
}

TEST(BloomSectionLoadTest, DetectsPayloadCorruptionAndRollsBackOffset)
{
    BloomSection bloom;
    ASSERT_TRUE(bloom.add_key("alpha", 5).is_ok());
    std::vector<std::byte> bytes = serialize(bloom);
    bytes.back() ^= std::byte{ 1 };

    MemoryReadableFile readable(std::move(bytes));
    std::uint64_t offset = 555;
    Result<BloomSection> result = BloomSection::load(readable, offset, 0);

    ASSERT_FALSE(result.is_ok());
    EXPECT_EQ(result.status.code, StatusCode::ChecksumMismatch);
    EXPECT_EQ(offset, 555u);
}

TEST(BloomSectionLoadTest, RejectsWrongBlockType)
{
    BloomSection bloom;
    std::vector<std::byte> bytes = serialize(bloom);
    bytes[0] = static_cast<std::byte>(BlockType::Index);

    MemoryReadableFile readable(std::move(bytes));
    std::uint64_t offset = 555;
    Result<BloomSection> result = BloomSection::load(readable, offset, 0);

    ASSERT_FALSE(result.is_ok());
    EXPECT_EQ(result.status.code, StatusCode::InvalidBlockType);
    EXPECT_EQ(offset, 555u);
}

TEST(BloomSectionLoadTest, RejectsWrongPayloadSize)
{
    BloomSection bloom;
    std::vector<std::byte> bytes = serialize(bloom);
    write_u32_le(bytes, 1, 1);

    MemoryReadableFile readable(std::move(bytes));
    std::uint64_t offset = 555;
    Result<BloomSection> result = BloomSection::load(readable, offset, 0);

    ASSERT_FALSE(result.is_ok());
    EXPECT_EQ(result.status.code, StatusCode::InvalidPayloadSize);
    EXPECT_EQ(offset, 555u);
}

TEST(BloomSectionLoadTest, RejectsUnsupportedHashCountEvenWithValidCRC)
{
    BloomSection bloom;
    std::vector<std::byte> bytes = serialize(bloom);
    write_u32_le(bytes, 17, BLOOM_HASH_COUNT + 1);
    refresh_serialized_payload_crc(bytes);

    MemoryReadableFile readable(std::move(bytes));
    std::uint64_t offset = 555;
    Result<BloomSection> result = BloomSection::load(readable, offset, 0);

    ASSERT_FALSE(result.is_ok());
    EXPECT_EQ(result.status.code, StatusCode::NotSupported);
    EXPECT_EQ(offset, 555u);
}

TEST(BloomSectionLoadTest, RejectsNonBooleanMaskEvenWithValidCRC)
{
    BloomSection bloom;
    ASSERT_TRUE(bloom.add_key("alpha", 5).is_ok());
    std::vector<std::byte> bytes = serialize(bloom);
    bytes.back() = std::byte{ 2 };
    refresh_serialized_payload_crc(bytes);

    MemoryReadableFile readable(std::move(bytes));
    std::uint64_t offset = 555;
    Result<BloomSection> result = BloomSection::load(readable, offset, 0);

    ASSERT_FALSE(result.is_ok());
    EXPECT_EQ(result.status.code, StatusCode::InvalidArgument);
    EXPECT_EQ(offset, 555u);
}