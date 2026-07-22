#include <gtest/gtest.h>

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
#include "record.h"
#include "sstable_entities.h"
#include "sstable_entities/data_section.h"
#include "sstable_entities/index_section.h"

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
            bytes_(
                static_cast<std::size_t>(initial_position),
                std::byte{ 0 }
            )
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
                    "test writable file offset mismatch"
                };
            }

            if (bytes_written_ + size > fail_after_bytes_) {
                return Status{
                    StatusCode::WriteFailed,
                    "injected test write failure"
                };
            }

            if (size == 0) {
                return Status::ok();
            }

            if (data == nullptr) {
                return Status{
                    StatusCode::NullPointer,
                    "test writable file received null data"
                };
            }

            const std::size_t required =
                static_cast<std::size_t>(position_) + size;
            if (bytes_.size() < required) {
                bytes_.resize(required);
            }

            std::memcpy(
                bytes_.data() + static_cast<std::size_t>(position_),
                data,
                size
            );

            position_ += size;
            tracked_offset += size;
            bytes_written_ += size;

            return Status::ok();
        }

        Status sync() override
        {
            return Status::ok();
        }

        Status close() override
        {
            return Status::ok();
        }

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
#ifndef _WIN32
        int descriptor_ = -1;
#endif
        std::uint64_t position_ = 0;
        std::size_t bytes_written_ = 0;
        std::size_t fail_after_bytes_ =
            std::numeric_limits<std::size_t>::max();
        std::vector<std::byte> bytes_;
    };

    struct StoredRecord
    {
        std::string key;
        std::string value;
        InternalRecord record{};

        StoredRecord(
            std::string key_bytes,
            std::string value_bytes,
            Type type,
            std::uint64_t sequence
        )
            : key(std::move(key_bytes)),
            value(std::move(value_bytes))
        {
            record.key_entry.data = key.empty() ? nullptr : key.data();
            record.key_entry.size = static_cast<std::uint32_t>(key.size());
            record.value_entry.data = value.empty() ? nullptr : value.data();
            record.value_entry.size =
                static_cast<std::uint32_t>(value.size());
            record.type = type;
            record.seq_num = sequence;
        }
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
}

TEST(DataSectionLayoutTest, UsesStableSerializedWidths)
{
    EXPECT_EQ(DataSection::Header::disk_size(), 9u);
    EXPECT_EQ(DataSection::Payload::fixed_part_disk_size(), 25u);
}

TEST(DataSectionTest, RejectsInvalidRecordTypeWithoutMutation)
{
    InternalRecord record{};
    record.type = static_cast<Type>(0xFFu);

    DataSection data;
    const Status status = data.add_payload(record);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::InvalidArgument);
    EXPECT_TRUE(data.data_blocks.empty());
}

TEST(DataSectionTest, RejectsNonEmptyNullPointersWithoutMutation)
{
    InternalRecord record{};
    record.key_entry.data = nullptr;
    record.key_entry.size = 1;
    record.type = Type::Put;

    DataSection data;
    const Status status = data.add_payload(record);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::NullPointer);
    EXPECT_TRUE(data.data_blocks.empty());
}

TEST(DataSectionTest, AcceptsPayloadThatExactlyFillsPhysicalBlock)
{
    constexpr std::size_t key_size = 1;
    constexpr std::size_t value_size =
        BLOCK_SIZE -
        DataSection::Header::disk_size() -
        DataSection::Payload::fixed_part_disk_size() -
        key_size;

    StoredRecord record(
        std::string(key_size, 'k'),
        std::string(value_size, 'v'),
        Type::Put,
        1
    );

    DataSection data;
    ASSERT_TRUE(data.add_payload(record.record).is_ok());

    ASSERT_EQ(data.data_blocks.size(), 1u);
    EXPECT_EQ(data.data_blocks.front().disk_size(), BLOCK_SIZE);
    EXPECT_EQ(
        data.data_blocks.front().header.payload_disk_size,
        BLOCK_SIZE - DataSection::Header::disk_size()
    );
}

TEST(DataSectionTest, SplitsRecordsAcrossPhysicalBlocks)
{
    StoredRecord first("a", std::string(1800, '1'), Type::Put, 1);
    StoredRecord second("b", std::string(1800, '2'), Type::Put, 2);
    StoredRecord third("c", std::string(1800, '3'), Type::Put, 3);

    DataSection data;
    ASSERT_TRUE(data.add_payload(first.record).is_ok());
    ASSERT_TRUE(data.add_payload(second.record).is_ok());
    ASSERT_TRUE(data.add_payload(third.record).is_ok());

    ASSERT_EQ(data.data_blocks.size(), 2u);
    EXPECT_EQ(data.data_blocks[0].payloads.size(), 2u);
    EXPECT_EQ(data.data_blocks[1].payloads.size(), 1u);
    EXPECT_LE(data.data_blocks[0].disk_size(), BLOCK_SIZE);
    EXPECT_LE(data.data_blocks[1].disk_size(), BLOCK_SIZE);
}

TEST(DataSectionTest, RebuildsStaleDerivedHeaderBeforeWrite)
{
    StoredRecord first("alpha", "one", Type::Put, 1);
    StoredRecord second("beta", "two", Type::Put, 2);

    DataSection data;
    ASSERT_TRUE(data.add_payload(first.record).is_ok());
    ASSERT_TRUE(data.add_payload(second.record).is_ok());

    DataSection::DataBlock& block = data.data_blocks.front();
    block.header.type = BlockType::Index;
    block.header.payload_disk_size = 1;
    block.header.crc32 = 2;

    MemoryWritableFile file;
    IndexSection index;
    std::uint64_t offset = 0;
    std::uint64_t data_offset = 999;

    const Status status = data.write(file, offset, index, data_offset);
    ASSERT_TRUE(status.is_ok()) << status.message;

    std::uint32_t recomputed_crc = 0;
    block.calculate_crc32(recomputed_crc);

    EXPECT_EQ(block.header.type, BlockType::Data);
    EXPECT_EQ(
        block.header.payload_disk_size,
        block.disk_size() - DataSection::Header::disk_size()
    );
    EXPECT_EQ(block.header.crc32, recomputed_crc);

    ASSERT_GE(file.bytes().size(), DataSection::Header::disk_size());
    EXPECT_EQ(
        std::to_integer<std::uint8_t>(file.bytes()[0]),
        static_cast<std::uint8_t>(BlockType::Data)
    );
    EXPECT_EQ(read_u32_le(file.bytes(), 1), block.header.payload_disk_size);
    EXPECT_EQ(read_u32_le(file.bytes(), 5), block.header.crc32);
}

TEST(DataSectionTest, AlignsBlocksAndBuildsOneIndexEntryPerBlock)
{
    StoredRecord first("a", std::string(1800, '1'), Type::Put, 1);
    StoredRecord second("b", std::string(1800, '2'), Type::Put, 2);
    StoredRecord third("c", std::string(1800, '3'), Type::Put, 3);

    DataSection data;
    ASSERT_TRUE(data.add_payload(first.record).is_ok());
    ASSERT_TRUE(data.add_payload(second.record).is_ok());
    ASSERT_TRUE(data.add_payload(third.record).is_ok());

    MemoryWritableFile file(24);
    IndexSection index;
    std::uint64_t offset = 24;
    std::uint64_t data_offset = 999;

    const Status status = data.write(file, offset, index, data_offset);
    ASSERT_TRUE(status.is_ok()) << status.message;

    EXPECT_EQ(data_offset, BLOCK_SIZE);
    ASSERT_EQ(index.payloads.size(), 2u);
    EXPECT_EQ(index.payloads[0].data_block_offset, BLOCK_SIZE);
    EXPECT_EQ(index.payloads[1].data_block_offset, 2u * BLOCK_SIZE);
    EXPECT_EQ(index.payloads[0].first_key_ptr, first.record.key_entry.data);
    EXPECT_EQ(index.payloads[0].last_key_ptr, second.record.key_entry.data);
    EXPECT_EQ(index.payloads[1].first_key_ptr, third.record.key_entry.data);
    EXPECT_EQ(index.payloads[1].last_key_ptr, third.record.key_entry.data);
}

TEST(DataSectionTest, RepeatedWriteReplacesDerivedIndexInsteadOfAppending)
{
    StoredRecord first("a", std::string(1800, '1'), Type::Put, 1);
    StoredRecord second("b", std::string(1800, '2'), Type::Put, 2);
    StoredRecord third("c", std::string(1800, '3'), Type::Put, 3);

    DataSection data;
    ASSERT_TRUE(data.add_payload(first.record).is_ok());
    ASSERT_TRUE(data.add_payload(second.record).is_ok());
    ASSERT_TRUE(data.add_payload(third.record).is_ok());

    IndexSection index;
    std::uint64_t data_offset = 0;

    MemoryWritableFile first_file;
    std::uint64_t first_offset = 0;
    ASSERT_TRUE(
        data.write(first_file, first_offset, index, data_offset).is_ok()
    );
    ASSERT_EQ(index.payloads.size(), 2u);

    MemoryWritableFile second_file;
    std::uint64_t second_offset = 0;
    ASSERT_TRUE(
        data.write(second_file, second_offset, index, data_offset).is_ok()
    );

    EXPECT_EQ(index.payloads.size(), 2u);
    EXPECT_EQ(index.payloads[0].data_block_offset, 0u);
    EXPECT_EQ(index.payloads[1].data_block_offset, BLOCK_SIZE);
}

TEST(DataSectionTest, RejectsManuallyInsertedEmptyBlockBeforeIO)
{
    DataSection data;
    data.init_new_block();

    std::string sentinel_key = "x";
    IndexSection index;
    index.add_index(
        777,
        1,
        1,
        sentinel_key.data(),
        sentinel_key.data()
    );

    MemoryWritableFile file;
    std::uint64_t offset = 0;
    std::uint64_t data_offset = 123;

    const Status status = data.write(file, offset, index, data_offset);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::InvalidState);
    EXPECT_EQ(offset, 0u);
    EXPECT_EQ(data_offset, 123u);
    ASSERT_EQ(index.payloads.size(), 1u);
    EXPECT_EQ(index.payloads.front().data_block_offset, 777u);
    EXPECT_TRUE(file.bytes().empty());
}

TEST(DataSectionPayloadTest, DoesNotSilentlyAlignAndSplitAcrossBlocks)
{
    StoredRecord record("a", "b", Type::Put, 1);
    const DataSection::Payload payload(record.record);

    constexpr std::uint64_t start = BLOCK_SIZE - 10;
    MemoryWritableFile file(start);
    std::uint64_t offset = start;

    const Status status = payload.write(file, offset);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::SizeExceedsBlockBoundary);
    EXPECT_EQ(offset, start);
    ASSERT_TRUE(file.current_position().is_ok());
    EXPECT_EQ(file.current_position().value, start);
}

TEST(DataSectionTest, FailedWritePreservesCallerDerivedMetadata)
{
    StoredRecord record("key", "value", Type::Put, 1);
    DataSection data;
    ASSERT_TRUE(data.add_payload(record.record).is_ok());

    std::string sentinel_key = "s";
    IndexSection index;
    index.add_index(
        987,
        1,
        1,
        sentinel_key.data(),
        sentinel_key.data()
    );

    MemoryWritableFile file(0, 20);
    std::uint64_t offset = 0;
    std::uint64_t data_offset = 321;

    const Status status = data.write(file, offset, index, data_offset);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::WriteFailed);
    EXPECT_EQ(data_offset, 321u);
    ASSERT_EQ(index.payloads.size(), 1u);
    EXPECT_EQ(index.payloads.front().data_block_offset, 987u);

    // Physical output and the tracked physical offset are intentionally not
    // rolled back by this layer.
    EXPECT_GT(offset, 0u);
}

TEST(DataSectionTest, EmptySectionIsNoOpAndClearsDerivedIndex)
{
    DataSection data;
    std::string sentinel_key = "s";
    IndexSection index;
    index.add_index(
        42,
        1,
        1,
        sentinel_key.data(),
        sentinel_key.data()
    );

    MemoryWritableFile file(24);
    std::uint64_t offset = 24;
    std::uint64_t data_offset = 999;

    const Status status = data.write(file, offset, index, data_offset);

    ASSERT_TRUE(status.is_ok()) << status.message;
    EXPECT_EQ(offset, 24u);
    EXPECT_EQ(data_offset, 24u);
    EXPECT_TRUE(index.payloads.empty());
    EXPECT_EQ(file.bytes().size(), 24u);
}


TEST(DataSectionSerializationTest, WritesCanonicalLittleEndianPayloadAndIndependentCRC)
{
    StoredRecord record(
        std::string("\x01\x02", 2),
        std::string("\x00\xFF", 2),
        Type::Put,
        0x0102030405060708ull
    );

    DataSection data;
    ASSERT_TRUE(data.add_payload(record.record).is_ok());

    MemoryWritableFile file;
    IndexSection index;
    std::uint64_t offset = 0;
    std::uint64_t data_offset = 999;

    const Status status = data.write(file, offset, index, data_offset);
    ASSERT_TRUE(status.is_ok()) << status.message;

    const std::vector<std::byte>& bytes = file.bytes();
    ASSERT_EQ(data_offset, 0u);
    ASSERT_EQ(bytes.size(), 38u);

    EXPECT_EQ(std::to_integer<std::uint8_t>(bytes[0]),
        static_cast<std::uint8_t>(BlockType::Data));
    EXPECT_EQ(read_u32_le(bytes, 1), 29u);

    EXPECT_EQ(read_u32_le(bytes, 9), 2u);
    EXPECT_EQ(read_u32_le(bytes, 13), 2u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(bytes[17]),
        static_cast<std::uint8_t>(Type::Put));
    EXPECT_EQ(read_u32_le(bytes, 18), 0u);
    EXPECT_EQ(read_u32_le(bytes, 22), 0u);

    const std::array<std::uint8_t, 8> expected_seq{
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01
    };
    for (std::size_t i = 0; i < expected_seq.size(); ++i) {
        EXPECT_EQ(
            std::to_integer<std::uint8_t>(bytes[26 + i]),
            expected_seq[i]
        );
    }

    EXPECT_EQ(std::to_integer<std::uint8_t>(bytes[34]), 0x01u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(bytes[35]), 0x02u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(bytes[36]), 0x00u);
    EXPECT_EQ(std::to_integer<std::uint8_t>(bytes[37]), 0xFFu);

    const std::uint32_t direct_crc = crc32_of(
        bytes.data() + DataSection::Header::disk_size(),
        29u
    );
    EXPECT_EQ(read_u32_le(bytes, 5), direct_crc);
}

TEST(DataSectionTest, RejectsDescendingKeysWithoutMutation)
{
    StoredRecord first("b", "one", Type::Put, 1);
    StoredRecord second("a", "two", Type::Put, 2);

    DataSection data;
    ASSERT_TRUE(data.add_payload(first.record).is_ok());

    const Status status = data.add_payload(second.record);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::InvalidArgument);
    ASSERT_EQ(data.data_blocks.size(), 1u);
    EXPECT_EQ(data.data_blocks.front().payloads.size(), 1u);
}

TEST(DataSectionTest, RequiresDescendingSequenceNumbersForEqualKeys)
{
    StoredRecord newest("same", "new", Type::Put, 20);
    StoredRecord older("same", "old", Type::Put, 10);
    StoredRecord wrongly_newer("same", "wrong", Type::Put, 30);

    DataSection data;
    ASSERT_TRUE(data.add_payload(newest.record).is_ok());
    ASSERT_TRUE(data.add_payload(older.record).is_ok());

    const Status status = data.add_payload(wrongly_newer.record);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::InvalidArgument);
    EXPECT_EQ(data.data_blocks.front().payloads.size(), 2u);
}

TEST(DataSectionTest, RejectsDuplicateInternalKeyAndSequence)
{
    StoredRecord first("same", "one", Type::Put, 7);
    StoredRecord duplicate("same", "two", Type::Tombstone, 7);

    DataSection data;
    ASSERT_TRUE(data.add_payload(first.record).is_ok());

    const Status status = data.add_payload(duplicate.record);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::Duplicate);
    EXPECT_EQ(data.data_blocks.front().payloads.size(), 1u);
}

TEST(DataSectionPayloadTest, RejectsUnknownFlagsAndReservedFields)
{
    StoredRecord record("key", "value", Type::Put, 1);
    DataSection::Payload payload(record.record);

    payload.flags = 1;
    Status status = payload.validate();
    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::InvalidArgument);

    payload.flags = 0;
    payload.reserved = 1;
    status = payload.validate();
    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::InvalidArgument);
}

TEST(DataSectionTest, DistinguishesLogicalBytesFromAlignedPhysicalSpan)
{
    StoredRecord first("a", std::string(1800, '1'), Type::Put, 3);
    StoredRecord second("b", std::string(1800, '2'), Type::Put, 2);
    StoredRecord third("c", std::string(1800, '3'), Type::Put, 1);

    DataSection data;
    ASSERT_TRUE(data.add_payload(first.record).is_ok());
    ASSERT_TRUE(data.add_payload(second.record).is_ok());
    ASSERT_TRUE(data.add_payload(third.record).is_ok());
    ASSERT_EQ(data.data_blocks.size(), 2u);

    const std::size_t logical =
        data.data_blocks[0].disk_size() + data.data_blocks[1].disk_size();
    const std::size_t physical =
        BLOCK_SIZE + data.data_blocks[1].disk_size();

    EXPECT_EQ(data.disk_size(), logical);
    EXPECT_EQ(data.physical_span(), physical);
    EXPECT_GT(data.physical_span(), data.disk_size());
}

TEST(DataSectionTest, RejectsOrderingCorruptionBeforeAnyIO)
{
    StoredRecord first("a", "one", Type::Put, 1);
    StoredRecord second("b", "two", Type::Put, 2);

    DataSection data;
    ASSERT_TRUE(data.add_payload(first.record).is_ok());
    ASSERT_TRUE(data.add_payload(second.record).is_ok());

    // Public state can be mutated, so write() must revalidate it.
    data.data_blocks.front().payloads[0].key_ptr = second.key.data();
    data.data_blocks.front().payloads[0].key_size =
        static_cast<std::uint32_t>(second.key.size());
    data.data_blocks.front().payloads[1].key_ptr = first.key.data();
    data.data_blocks.front().payloads[1].key_size =
        static_cast<std::uint32_t>(first.key.size());

    MemoryWritableFile file;
    IndexSection index;
    std::uint64_t offset = 0;
    std::uint64_t data_offset = 123;

    const Status status = data.write(file, offset, index, data_offset);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::InvalidArgument);
    EXPECT_EQ(offset, 0u);
    EXPECT_EQ(data_offset, 123u);
    EXPECT_TRUE(file.bytes().empty());
    EXPECT_TRUE(index.payloads.empty());
}