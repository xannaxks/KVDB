#include <gtest/gtest.h>

#include "endian_io.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace
{
    class MemoryWritableFile final : public WritableFile
    {
    public:
        std::vector<std::byte> bytes;
        std::uint64_t cursor = 0;
        bool fail_current_position = false;
        std::size_t append_calls = 0;
        std::size_t fail_on_append_call = std::numeric_limits<std::size_t>::max();

        Status append(
            const void* data,
            std::size_t size,
            std::uint64_t& tracked_offset
        ) override
        {
            ++append_calls;

            if (append_calls == fail_on_append_call) {
                return Status{ StatusCode::WriteFailed, "injected append failure" };
            }

            if (tracked_offset != cursor) {
                return Status{ StatusCode::InvalidOffset, "tracked offset differs from cursor" };
            }

            if (size == 0) {
                return Status::ok();
            }

            if (data == nullptr) {
                return Status{ StatusCode::InvalidArgument, "null append buffer" };
            }

            const auto* begin = static_cast<const std::byte*>(data);
            bytes.insert(bytes.end(), begin, begin + size);
            cursor += size;
            tracked_offset += size;
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
            if (fail_current_position) {
                return Result<std::uint64_t>::fail(
                    Status{ StatusCode::GetPositionFailed, "injected position failure" }
                );
            }

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

        Status sync_parent_directory() override
        {
            return Status::ok();
        }

        std::filesystem::path parent_directory() override
        {
            return {};
        }
    };

    class MemoryReadableFile final : public ReadableFile
    {
    public:
        explicit MemoryReadableFile(std::vector<std::byte> input)
            : bytes(std::move(input))
        {
        }

        std::vector<std::byte> bytes;
        bool fail_reads = false;
        std::size_t max_chunk_size = std::numeric_limits<std::size_t>::max();

        Status read_at(
            std::uint64_t offset,
            void* buffer,
            std::size_t size,
            std::size_t& bytes_read
        ) override
        {
            bytes_read = 0;

            if (fail_reads) {
                return Status{ StatusCode::ReadFailed, "injected read failure" };
            }

            if (size == 0) {
                return Status::ok();
            }

            if (buffer == nullptr) {
                return Status{ StatusCode::InvalidArgument, "null read buffer" };
            }

            if (offset >= bytes.size()) {
                return Status::ok();
            }

            const std::size_t available = bytes.size() - static_cast<std::size_t>(offset);
            bytes_read = std::min({ size, available, max_chunk_size });
            std::memcpy(buffer, bytes.data() + offset, bytes_read);
            return Status::ok();
        }

        Status close() override
        {
            return Status::ok();
        }

        Status get_file_size(std::uint64_t& size_out) override
        {
            size_out = bytes.size();
            return Status::ok();
        }
    };

    std::vector<std::byte> make_bytes(std::initializer_list<unsigned int> values)
    {
        std::vector<std::byte> result;
        result.reserve(values.size());
        for (unsigned int value : values) {
            result.push_back(static_cast<std::byte>(value));
        }
        return result;
    }
}

TEST(EndianBufferIOTest, IntegerEncodersAppendExpectedLittleEndianBytes)
{
    std::vector<std::byte> out{ std::byte{0xEE} };

    kvdb::endian::put_u8(out, 0xABu);
    kvdb::endian::put_u16_le(out, 0x1234u);
    kvdb::endian::put_u32_le(out, 0x12345678u);
    kvdb::endian::put_u64_le(out, 0x1122334455667788ull);

    EXPECT_EQ(out, make_bytes({
        0xEE,
        0xAB,
        0x34, 0x12,
        0x78, 0x56, 0x34, 0x12,
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11
        }));
}

TEST(EndianBufferIOTest, SizedBytesAppendU32LengthThenPayload)
{
    std::vector<std::byte> out;
    const std::array payload{
        std::byte{'c'},
        std::byte{'a'},
        std::byte{'t'}
    };

    const Status status = kvdb::endian::put_bytes_with_u32_size(out, payload);

    ASSERT_TRUE(status.is_ok()) << status.message;
    EXPECT_EQ(out, make_bytes({ 0x03, 0x00, 0x00, 0x00, 'c', 'a', 't' }));
}

TEST(BlockIOWriteTest, WritesIntegersInLittleEndianAndAdvancesOffset)
{
    MemoryWritableFile file;
    std::uint64_t offset = 0;
    constexpr std::uint32_t block_size = 64;

    ASSERT_TRUE(kvdb::blockio::write_u8_t(file, 0xABu, offset, block_size).is_ok());
    ASSERT_TRUE(kvdb::blockio::write_u16_t_le(file, 0x1234u, offset, block_size).is_ok());
    ASSERT_TRUE(kvdb::blockio::write_u32_t_le(file, 0x12345678u, offset, block_size).is_ok());
    ASSERT_TRUE(kvdb::blockio::write_u64_t_le(file, 0x1122334455667788ull, offset, block_size).is_ok());

    EXPECT_EQ(offset, 15u);
    EXPECT_EQ(file.cursor, offset);
    EXPECT_EQ(file.bytes, make_bytes({
        0xAB,
        0x34, 0x12,
        0x78, 0x56, 0x34, 0x12,
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11
        }));
}

TEST(BlockIOWriteTest, WritesConstByteSpan)
{
    MemoryWritableFile file;
    std::uint64_t offset = 0;
    const std::array payload{
        std::byte{0x10},
        std::byte{0x20},
        std::byte{0x30}
    };

    const Status status = kvdb::blockio::write_bytes(file, payload, offset, 16);

    ASSERT_TRUE(status.is_ok()) << status.message;
    EXPECT_EQ(offset, payload.size());
    EXPECT_EQ(file.bytes, std::vector<std::byte>(payload.begin(), payload.end()));
}

TEST(BlockIOWriteTest, RejectsTrackedOffsetMismatchInReleaseBuildsToo)
{
    MemoryWritableFile file;
    std::uint64_t offset = 1;

    const Status status = kvdb::blockio::write_u32_t_le(file, 0x12345678u, offset, 16);

    EXPECT_EQ(status.code, StatusCode::InvalidOffset);
    EXPECT_EQ(offset, 1u);
    EXPECT_TRUE(file.bytes.empty());
    EXPECT_EQ(file.append_calls, 0u);
}

TEST(BlockIOWriteTest, PropagatesCurrentPositionFailure)
{
    MemoryWritableFile file;
    file.fail_current_position = true;
    std::uint64_t offset = 0;

    const Status status = kvdb::blockio::write_u8_t(file, 0x12u, offset, 16);

    EXPECT_EQ(status.code, StatusCode::GetPositionFailed);
    EXPECT_EQ(offset, 0u);
    EXPECT_TRUE(file.bytes.empty());
}

TEST(BlockIOWriteTest, AlignsToNextBlockBeforeWritingCrossBoundaryValue)
{
    MemoryWritableFile file;
    std::uint64_t offset = 0;
    const std::array prefix{
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
        std::byte{0x04}, std::byte{0x05}, std::byte{0x06},
        std::byte{0x07}
    };

    ASSERT_TRUE(kvdb::blockio::write_bytes(file, prefix, offset, 8).is_ok());
    ASSERT_TRUE(kvdb::blockio::write_u16_t_le(file, 0x1234u, offset, 8).is_ok());

    EXPECT_EQ(offset, 10u);
    EXPECT_EQ(file.bytes, make_bytes({
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0xCD,
        0x34, 0x12
        }));
}

TEST(BlockIOWriteTest, PropagatesAppendFailureWithoutAdvancingOffset)
{
    MemoryWritableFile file;
    file.fail_on_append_call = 1;
    std::uint64_t offset = 0;

    const Status status = kvdb::blockio::write_u32_t_le(file, 0x12345678u, offset, 16);

    EXPECT_EQ(status.code, StatusCode::WriteFailed);
    EXPECT_EQ(offset, 0u);
    EXPECT_EQ(file.cursor, 0u);
    EXPECT_TRUE(file.bytes.empty());
}

TEST(BlockIOWriteTest, KeepsCommittedPaddingWhenFollowingValueWriteFails)
{
    MemoryWritableFile file;
    std::uint64_t offset = 0;
    const std::array prefix{
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
        std::byte{0x04}, std::byte{0x05}, std::byte{0x06},
        std::byte{0x07}
    };

    ASSERT_TRUE(kvdb::blockio::write_bytes(file, prefix, offset, 8).is_ok());

    // append #2 writes alignment padding; append #3 is the u16 itself.
    file.fail_on_append_call = 3;
    const Status status = kvdb::blockio::write_u16_t_le(file, 0x1234u, offset, 8);

    EXPECT_EQ(status.code, StatusCode::WriteFailed);
    EXPECT_EQ(offset, 8u);
    EXPECT_EQ(file.cursor, 8u);
    EXPECT_EQ(file.bytes, make_bytes({
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0xCD
        }));
}

TEST(BlockIOWriteTest, RejectsZeroBlockSizeWithoutMutation)
{
    MemoryWritableFile file;
    std::uint64_t offset = 0;

    const Status status = kvdb::blockio::write_u16_t_le(file, 0x1234u, offset, 0);

    EXPECT_EQ(status.code, StatusCode::InvalidArgument);
    EXPECT_EQ(offset, 0u);
    EXPECT_TRUE(file.bytes.empty());
}

TEST(BlockIOWriteTest, RejectsPayloadLargerThanBlockBeforeWritingSizedPrefix)
{
    MemoryWritableFile file;
    std::uint64_t offset = 0;
    const std::array payload{
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
        std::byte{0x04}, std::byte{0x05}
    };

    const Status status = kvdb::blockio::write_bytes_with_u32_size(
        file,
        payload,
        offset,
        4
    );

    EXPECT_EQ(status.code, StatusCode::SizeExceedsBlockSize);
    EXPECT_EQ(offset, 0u);
    EXPECT_TRUE(file.bytes.empty());
}

TEST(BlockIOReadTest, ReadsIntegersInLittleEndianAndAdvancesOffset)
{
    MemoryReadableFile file(make_bytes({
        0xAB,
        0x34, 0x12,
        0x78, 0x56, 0x34, 0x12,
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11
        }));
    std::uint64_t offset = 0;
    std::uint8_t u8 = 0;
    std::uint16_t u16 = 0;
    std::uint32_t u32 = 0;
    std::uint64_t u64 = 0;

    ASSERT_TRUE(kvdb::blockio::read_u8_t(file, u8, offset, 64).is_ok());
    ASSERT_TRUE(kvdb::blockio::read_u16_t_le(file, u16, offset, 64).is_ok());
    ASSERT_TRUE(kvdb::blockio::read_u32_t_le(file, u32, offset, 64).is_ok());
    ASSERT_TRUE(kvdb::blockio::read_u64_t_le(file, u64, offset, 64).is_ok());

    EXPECT_EQ(u8, 0xABu);
    EXPECT_EQ(u16, 0x1234u);
    EXPECT_EQ(u32, 0x12345678u);
    EXPECT_EQ(u64, 0x1122334455667788ull);
    EXPECT_EQ(offset, 15u);
}

TEST(BlockIOReadTest, ReadExactHandlesMultipleShortReads)
{
    MemoryReadableFile file(make_bytes({
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11
        }));
    file.max_chunk_size = 2;
    std::uint64_t offset = 0;
    std::uint64_t value = 0;

    const Status status = kvdb::blockio::read_u64_t_le(file, value, offset, 16);

    ASSERT_TRUE(status.is_ok()) << status.message;
    EXPECT_EQ(value, 0x1122334455667788ull);
    EXPECT_EQ(offset, 8u);
}

TEST(BlockIOReadTest, AlignsToNextBlockBeforeReadingCrossBoundaryValue)
{
    MemoryReadableFile file(make_bytes({
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xCD,
        0x34, 0x12
        }));
    std::uint64_t offset = 7;
    std::uint16_t value = 0;

    const Status status = kvdb::blockio::read_u16_t_le(file, value, offset, 8);

    ASSERT_TRUE(status.is_ok()) << status.message;
    EXPECT_EQ(value, 0x1234u);
    EXPECT_EQ(offset, 10u);
}

TEST(BlockIOReadTest, TruncatedIntegerPreservesDestinationAndOriginalOffset)
{
    MemoryReadableFile file(make_bytes({ 0x78, 0x56, 0x34 }));
    std::uint64_t offset = 0;
    std::uint32_t value = 0xDEADBEEFu;

    const Status status = kvdb::blockio::read_u32_t_le(file, value, offset, 64);

    EXPECT_EQ(status.code, StatusCode::UnexpectedEOF);
    EXPECT_EQ(offset, 0u);
    EXPECT_EQ(value, 0xDEADBEEFu);
}

TEST(BlockIOReadTest, FailedReadAfterAlignmentRestoresOriginalOffset)
{
    MemoryReadableFile file(make_bytes({
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCD
        }));
    std::uint64_t offset = 7;
    std::uint16_t value = 0xBEEFu;

    const Status status = kvdb::blockio::read_u16_t_le(file, value, offset, 8);

    EXPECT_EQ(status.code, StatusCode::UnexpectedEOF);
    EXPECT_EQ(offset, 7u);
    EXPECT_EQ(value, 0xBEEFu);
}

TEST(BlockIOReadTest, RejectsNullBufferForNonZeroRead)
{
    MemoryReadableFile file(make_bytes({ 0x01 }));
    std::uint64_t offset = 0;

    const Status status = kvdb::blockio::read_bytes(file, nullptr, 1, offset, 8);

    EXPECT_EQ(status.code, StatusCode::InvalidArgument);
    EXPECT_EQ(offset, 0u);
}

TEST(BlockIOReadTest, AllowsNullBufferForZeroLengthRead)
{
    MemoryReadableFile file({});
    std::uint64_t offset = 3;

    const Status status = kvdb::blockio::read_bytes(file, nullptr, 0, offset, 8);

    EXPECT_TRUE(status.is_ok()) << status.message;
    EXPECT_EQ(offset, 3u);
}

TEST(BlockIOReadTest, RejectsZeroBlockSize)
{
    MemoryReadableFile file(make_bytes({ 0x01 }));
    std::uint64_t offset = 0;
    std::uint8_t value = 0xFFu;

    const Status status = kvdb::blockio::read_u8_t(file, value, offset, 0);

    EXPECT_EQ(status.code, StatusCode::InvalidArgument);
    EXPECT_EQ(offset, 0u);
    EXPECT_EQ(value, 0xFFu);
}

TEST(BlockIOSizedBytesTest, RoundTripsPayload)
{
    MemoryWritableFile writer;
    std::uint64_t write_offset = 0;
    const std::array payload{
        std::byte{0xFF},
        std::byte{0x00},
        std::byte{0xCD},
        std::byte{0x7F}
    };

    ASSERT_TRUE(
        kvdb::blockio::write_bytes_with_u32_size(writer, payload, write_offset, 16).is_ok()
    );

    MemoryReadableFile reader(writer.bytes);
    std::array<std::byte, 8> destination{};
    std::uint32_t bytes_read = 0;
    std::uint64_t read_offset = 0;

    const Status status = kvdb::blockio::read_bytes_with_u32_size(
        reader,
        destination,
        bytes_read,
        read_offset,
        16
    );

    ASSERT_TRUE(status.is_ok()) << status.message;
    EXPECT_EQ(bytes_read, payload.size());
    EXPECT_EQ(read_offset, write_offset);
    EXPECT_TRUE(std::equal(
        payload.begin(),
        payload.end(),
        destination.begin()
    ));
}

TEST(BlockIOSizedBytesTest, EmptyPayloadConsumesOnlySizePrefix)
{
    MemoryReadableFile reader(make_bytes({ 0x00, 0x00, 0x00, 0x00 }));
    std::span<std::byte> empty_buffer;
    std::uint32_t bytes_read = 99;
    std::uint64_t offset = 0;

    const Status status = kvdb::blockio::read_bytes_with_u32_size(
        reader,
        empty_buffer,
        bytes_read,
        offset,
        16
    );

    ASSERT_TRUE(status.is_ok()) << status.message;
    EXPECT_EQ(bytes_read, 0u);
    EXPECT_EQ(offset, 4u);
}

TEST(BlockIOSizedBytesTest, BufferTooSmallRestoresOffsetAndDoesNotPublishLength)
{
    MemoryReadableFile reader(make_bytes({
        0x03, 0x00, 0x00, 0x00,
        0x10, 0x20, 0x30
        }));
    std::array<std::byte, 2> destination{};
    std::uint32_t bytes_read = 99;
    std::uint64_t offset = 0;

    const Status status = kvdb::blockio::read_bytes_with_u32_size(
        reader,
        destination,
        bytes_read,
        offset,
        16
    );

    EXPECT_EQ(status.code, StatusCode::BufferTooSmall);
    EXPECT_EQ(offset, 0u);
    EXPECT_EQ(bytes_read, 99u);
}

TEST(BlockIOSizedBytesTest, TruncatedPayloadRestoresWholeRecordOffset)
{
    MemoryReadableFile reader(make_bytes({
        0x03, 0x00, 0x00, 0x00,
        0x10, 0x20
        }));
    std::array<std::byte, 3> destination{};
    std::uint32_t bytes_read = 99;
    std::uint64_t offset = 0;

    const Status status = kvdb::blockio::read_bytes_with_u32_size(
        reader,
        destination,
        bytes_read,
        offset,
        16
    );

    EXPECT_EQ(status.code, StatusCode::UnexpectedEOF);
    EXPECT_EQ(offset, 0u);
    EXPECT_EQ(bytes_read, 99u);
}