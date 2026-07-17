#include <gtest/gtest.h>

#include "wal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "file_helpers.h"

namespace
{

#define ASSERT_STATUS_OK(expression)                                            \
    do {                                                                        \
        const Status status__ = (expression);                                   \
        ASSERT_TRUE(status__.is_ok())                                           \
            << "status code=" << static_cast<int>(status__.code)               \
            << ", message=" << status__.message;                               \
    } while (false)

#define EXPECT_STATUS_CODE(expression, expected_code)                           \
    do {                                                                        \
        const Status status__ = (expression);                                   \
        EXPECT_FALSE(status__.is_ok());                                         \
        EXPECT_EQ(status__.code, (expected_code))                               \
            << "message=" << status__.message;                                 \
    } while (false)

    // Keep Arena construction isolated here. If your Arena requires options,
    // only this helper should need changing.
    std::unique_ptr<Arena> make_test_arena()
    {
        return std::make_unique<Arena>();
    }

    std::vector<std::byte> bytes_of(std::string_view text)
    {
        std::vector<std::byte> bytes;
        bytes.reserve(text.size());

        for (const unsigned char ch : text) {
            bytes.push_back(static_cast<std::byte>(ch));
        }

        return bytes;
    }

    std::vector<std::byte> patterned_bytes(
        std::size_t size,
        std::uint8_t seed = 0x41
    )
    {
        std::vector<std::byte> bytes(size);

        for (std::size_t i = 0; i < size; ++i) {
            bytes[i] = static_cast<std::byte>(
                static_cast<std::uint8_t>(seed + (i % 113u))
                );
        }

        return bytes;
    }

    void append_u32_le(std::vector<std::byte>& out, std::uint32_t value)
    {
        out.push_back(static_cast<std::byte>(value & 0xffu));
        out.push_back(static_cast<std::byte>((value >> 8u) & 0xffu));
        out.push_back(static_cast<std::byte>((value >> 16u) & 0xffu));
        out.push_back(static_cast<std::byte>((value >> 24u) & 0xffu));
    }

    std::vector<std::byte> encode_logical_payload(
        std::span<const std::byte> key,
        std::span<const std::byte> value
    )
    {
        EXPECT_LE(key.size(), std::numeric_limits<std::uint32_t>::max());
        EXPECT_LE(value.size(), std::numeric_limits<std::uint32_t>::max());

        std::vector<std::byte> payload;
        payload.reserve(8u + key.size() + value.size());

        append_u32_le(payload, static_cast<std::uint32_t>(key.size()));
        append_u32_le(payload, static_cast<std::uint32_t>(value.size()));
        payload.insert(payload.end(), key.begin(), key.end());
        payload.insert(payload.end(), value.begin(), value.end());

        return payload;
    }

    struct OwnedRecord
    {
        std::vector<std::byte> key;
        std::vector<std::byte> value;
        InternalRecord record{};

        OwnedRecord(
            std::uint64_t seq_num,
            ::Type type,
            std::vector<std::byte> key_bytes,
            std::vector<std::byte> value_bytes
        )
            : key(std::move(key_bytes)),
            value(std::move(value_bytes))
        {
            record.seq_num = seq_num;
            record.type = type;
            bind_storage();
        }

        OwnedRecord(const OwnedRecord&) = delete;
        OwnedRecord& operator=(const OwnedRecord&) = delete;

        OwnedRecord(OwnedRecord&& other) noexcept
            : key(std::move(other.key)),
            value(std::move(other.value)),
            record(other.record)
        {
            bind_storage();
            other.record.key_entry.data = nullptr;
            other.record.key_entry.size = 0;
            other.record.value_entry.data = nullptr;
            other.record.value_entry.size = 0;
        }

        OwnedRecord& operator=(OwnedRecord&& other) noexcept
        {
            if (this == &other) {
                return *this;
            }

            key = std::move(other.key);
            value = std::move(other.value);
            record = other.record;
            bind_storage();

            other.record.key_entry.data = nullptr;
            other.record.key_entry.size = 0;
            other.record.value_entry.data = nullptr;
            other.record.value_entry.size = 0;

            return *this;
        }

    private:
        void bind_storage()
        {
            record.key_entry.data = key.empty() ? nullptr : key.data();
            record.key_entry.size = static_cast<std::uint32_t>(key.size());

            record.value_entry.data = value.empty() ? nullptr : value.data();
            record.value_entry.size = static_cast<std::uint32_t>(value.size());
        }
    };

    std::span<const std::byte> entry_bytes(const auto& entry)
    {
        if (entry.size == 0) {
            return {};
        }

        return {
            static_cast<const std::byte*>(entry.data),
            static_cast<std::size_t>(entry.size)
        };
    }

    void expect_record_equals(
        const InternalRecord& actual,
        const OwnedRecord& expected
    )
    {
        EXPECT_EQ(actual.seq_num, expected.record.seq_num);
        EXPECT_EQ(actual.type, expected.record.type);

        const auto actual_key = entry_bytes(actual.key_entry);
        const auto actual_value = entry_bytes(actual.value_entry);

        ASSERT_EQ(actual_key.size(), expected.key.size());
        ASSERT_EQ(actual_value.size(), expected.value.size());

        EXPECT_TRUE(std::equal(
            actual_key.begin(),
            actual_key.end(),
            expected.key.begin()
        ));

        EXPECT_TRUE(std::equal(
            actual_value.begin(),
            actual_value.end(),
            expected.value.begin()
        ));
    }

    ::testing::AssertionResult finalize_fragment(Fragment& fragment)
    {
        fragment.header.header_size = Fragment::Header::disk_size();
        fragment.header.fragment_size =
            static_cast<std::uint32_t>(fragment.payload.bytes.size());

        const Status status = fragment.compute_crc32();
        if (!status.is_ok()) {
            return ::testing::AssertionFailure()
                << "failed to compute fragment CRC: " << status.message;
        }

        return ::testing::AssertionSuccess();
    }

    ::testing::AssertionResult write_manual_wal(
        const std::filesystem::path& path,
        std::uint32_t wal_id,
        std::uint64_t start_seq,
        std::vector<Fragment> fragments
    )
    {
        auto open_result = open_writable_file(path);
        if (!open_result.is_ok()) {
            return ::testing::AssertionFailure()
                << "open_writable_file failed: " << open_result.status.message;
        }

        if (!open_result.value) {
            return ::testing::AssertionFailure()
                << "open_writable_file returned a null file";
        }

        std::unique_ptr<WritableFile> file = std::move(open_result.value);
        std::uint64_t offset = 0;

        WALFileHeader header(wal_id, start_seq);
        Status status = header.write(*file, offset);
        if (!status.is_ok()) {
            (void)file->close();
            return ::testing::AssertionFailure()
                << "header write failed: " << status.message;
        }

        for (Fragment& fragment : fragments) {
            status = fragment.write(*file, offset);
            if (!status.is_ok()) {
                (void)file->close();
                return ::testing::AssertionFailure()
                    << "fragment write failed: " << status.message;
            }
        }

        status = file->sync();
        if (!status.is_ok()) {
            (void)file->close();
            return ::testing::AssertionFailure()
                << "sync failed: " << status.message;
        }

        status = file->close();
        if (!status.is_ok()) {
            return ::testing::AssertionFailure()
                << "close failed: " << status.message;
        }

        return ::testing::AssertionSuccess();
    }

    ::testing::AssertionResult xor_file_byte(
        const std::filesystem::path& path,
        std::uint64_t offset,
        std::uint8_t mask = 0x80u
    )
    {
        std::fstream stream(
            path,
            std::ios::binary | std::ios::in | std::ios::out
        );

        if (!stream) {
            return ::testing::AssertionFailure()
                << "failed to open file for byte corruption";
        }

        stream.seekg(static_cast<std::streamoff>(offset));

        char byte = 0;
        stream.read(&byte, 1);
        if (stream.gcount() != 1) {
            return ::testing::AssertionFailure()
                << "could not read byte at offset " << offset;
        }

        byte = static_cast<char>(
            static_cast<unsigned char>(byte) ^ mask
            );

        stream.seekp(static_cast<std::streamoff>(offset));
        stream.write(&byte, 1);
        stream.flush();

        if (!stream) {
            return ::testing::AssertionFailure()
                << "could not write corrupted byte at offset " << offset;
        }

        return ::testing::AssertionSuccess();
    }

    class WALTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            const auto ticks = std::chrono::steady_clock::now()
                .time_since_epoch()
                .count();
            const std::uint64_t id = next_id_.fetch_add(1);

            path_ = std::filesystem::temp_directory_path() /
                ("kvdb_wal_test_" + std::to_string(ticks) + "_" +
                    std::to_string(id) + ".wal");
        }

        void TearDown() override
        {
            std::error_code error;
            std::filesystem::remove(path_, error);
        }

        std::filesystem::path path_;

    private:
        static inline std::atomic<std::uint64_t> next_id_{ 0 };
    };

    TEST_F(WALTest, HeaderConstructorProducesSelfConsistentCrc)
    {
        WALFileHeader header(17u, 1001u);

        EXPECT_TRUE(header.self_check());
        EXPECT_EQ(
            header.header_crc32,
            WALFileHeader::compute_crc32(header)
        );
    }

    TEST_F(WALTest, HeaderOnlyWalRecoversCleanly)
    {
        constexpr std::uint32_t wal_id = 9;

        WALWriter writer;
        ASSERT_STATUS_OK(writer.create(path_, wal_id, 77));
        ASSERT_STATUS_OK(writer.close());

        auto arena = make_test_arena();
        auto recovered = WAL::recover(path_, wal_id, *arena);

        ASSERT_TRUE(recovered.is_ok()) << recovered.status.message;
        ASSERT_TRUE(recovered.value.header.has_value());
        EXPECT_EQ(recovered.value.header->wal_id, wal_id);
        EXPECT_EQ(recovered.value.header->start_seq, 77u);
        EXPECT_TRUE(recovered.value.records.empty());
        EXPECT_TRUE(recovered.value.ok);
        EXPECT_FALSE(recovered.value.had_torn_tail);
        EXPECT_FALSE(recovered.value.had_corruption);
    }

    TEST_F(WALTest, SinglePutRoundTrips)
    {
        constexpr std::uint32_t wal_id = 3;
        OwnedRecord input(
            101,
            ::Type::Put,
            bytes_of("alpha"),
            bytes_of("value-1")
        );

        WALWriter writer;
        ASSERT_STATUS_OK(writer.create(path_, wal_id, input.record.seq_num));
        ASSERT_STATUS_OK(writer.write(input.record));
        ASSERT_STATUS_OK(writer.close());

        auto arena = make_test_arena();
        auto recovered = WAL::recover(path_, wal_id, *arena);

        ASSERT_TRUE(recovered.is_ok()) << recovered.status.message;
        ASSERT_EQ(recovered.value.records.size(), 1u);
        expect_record_equals(recovered.value.records[0], input);
        EXPECT_TRUE(recovered.value.ok);
        EXPECT_FALSE(recovered.value.had_torn_tail);
        EXPECT_FALSE(recovered.value.had_corruption);
    }

    TEST_F(WALTest, TombstoneWithEmptyValueRoundTrips)
    {
        constexpr std::uint32_t wal_id = 4;
        OwnedRecord input(
            202,
            ::Type::Tombstone,
            bytes_of("deleted-key"),
            {}
        );

        WALWriter writer;
        ASSERT_STATUS_OK(writer.create(path_, wal_id, input.record.seq_num));
        ASSERT_STATUS_OK(writer.write(input.record));
        ASSERT_STATUS_OK(writer.close());

        auto arena = make_test_arena();
        auto recovered = WAL::recover(path_, wal_id, *arena);

        ASSERT_TRUE(recovered.is_ok()) << recovered.status.message;
        ASSERT_EQ(recovered.value.records.size(), 1u);
        expect_record_equals(recovered.value.records[0], input);
    }

    TEST_F(WALTest, MultipleRecordsPreserveOrderAndBinaryBytes)
    {
        constexpr std::uint32_t wal_id = 5;

        OwnedRecord first(
            11,
            ::Type::Put,
            { std::byte{0x00}, std::byte{0xff}, std::byte{0x41} },
            { std::byte{0x7f}, std::byte{0x00} }
        );
        OwnedRecord second(
            12,
            ::Type::Put,
            bytes_of("second"),
            patterned_bytes(300, 0x10)
        );
        OwnedRecord third(
            13,
            ::Type::Tombstone,
            bytes_of("third"),
            {}
        );

        WALWriter writer;
        ASSERT_STATUS_OK(writer.create(path_, wal_id, first.record.seq_num));
        ASSERT_STATUS_OK(writer.write(first.record));
        ASSERT_STATUS_OK(writer.write(second.record));
        ASSERT_STATUS_OK(writer.write(third.record));
        ASSERT_STATUS_OK(writer.close());

        auto arena = make_test_arena();
        auto recovered = WAL::recover(path_, wal_id, *arena);

        ASSERT_TRUE(recovered.is_ok()) << recovered.status.message;
        ASSERT_EQ(recovered.value.records.size(), 3u);
        expect_record_equals(recovered.value.records[0], first);
        expect_record_equals(recovered.value.records[1], second);
        expect_record_equals(recovered.value.records[2], third);
    }

    TEST_F(WALTest, LargeRecordIsFragmentedAndReassembled)
    {
        constexpr std::uint32_t wal_id = 6;

        OwnedRecord input(
            500,
            ::Type::Put,
            patterned_bytes(WAL_FILE_BLOCK_SIZE / 2u, 0x22),
            patterned_bytes(WAL_FILE_BLOCK_SIZE * 3u + 137u, 0x61)
        );

        WALWriter writer;
        ASSERT_STATUS_OK(writer.create(path_, wal_id, input.record.seq_num));
        ASSERT_STATUS_OK(writer.write(input.record));

        EXPECT_GT(writer.offset(), WAL_FILE_BLOCK_SIZE * 3u);

        ASSERT_STATUS_OK(writer.close());

        auto arena = make_test_arena();
        auto recovered = WAL::recover(path_, wal_id, *arena);

        ASSERT_TRUE(recovered.is_ok()) << recovered.status.message;
        ASSERT_EQ(recovered.value.records.size(), 1u);
        expect_record_equals(recovered.value.records[0], input);
    }

    TEST_F(WALTest, WriterSkipsBlockWhenExactlyOneFragmentHeaderRemains)
    {
        constexpr std::uint32_t wal_id = 7;
        constexpr std::size_t prefix_size = 8;
        constexpr std::size_t fragment_header = Fragment::Header::disk_size();
        constexpr std::size_t file_header = WALFileHeader::disk_size();

        static_assert(
            WAL_FILE_BLOCK_SIZE > file_header + 2u * fragment_header + prefix_size
            );

        // This first record ends with exactly Fragment::Header::disk_size()
        // bytes remaining in the first block.
        constexpr std::size_t first_payload_size =
            WAL_FILE_BLOCK_SIZE - file_header - 2u * fragment_header;
        constexpr std::size_t first_key_size =
            first_payload_size - prefix_size;

        OwnedRecord first(
            1,
            ::Type::Put,
            patterned_bytes(first_key_size, 0x31),
            {}
        );
        OwnedRecord second(
            2,
            ::Type::Put,
            bytes_of("next"),
            bytes_of("record")
        );

        WALWriter writer;
        ASSERT_STATUS_OK(writer.create(path_, wal_id, first.record.seq_num));
        ASSERT_STATUS_OK(writer.write(first.record));

        EXPECT_EQ(
            writer.offset() % WAL_FILE_BLOCK_SIZE,
            WAL_FILE_BLOCK_SIZE - fragment_header
        );

        // Regression: the current implementation returns InvariantViolation here
        // because it asks whether the header fits, not whether header + >=1 payload
        // byte fits.
        ASSERT_STATUS_OK(writer.write(second.record));
        ASSERT_STATUS_OK(writer.close());

        auto arena = make_test_arena();
        auto recovered = WAL::recover(path_, wal_id, *arena);

        ASSERT_TRUE(recovered.is_ok()) << recovered.status.message;
        ASSERT_EQ(recovered.value.records.size(), 2u);
        expect_record_equals(recovered.value.records[0], first);
        expect_record_equals(recovered.value.records[1], second);
    }

    TEST_F(WALTest, LoaderSkipsPaddingWhenExactlyOneFragmentHeaderRemains)
    {
        constexpr std::uint32_t wal_id = 8;
        constexpr std::size_t prefix_size = 8;
        constexpr std::size_t fragment_header = Fragment::Header::disk_size();
        constexpr std::size_t file_header = WALFileHeader::disk_size();

        constexpr std::size_t first_payload_size =
            WAL_FILE_BLOCK_SIZE - file_header - 2u * fragment_header;
        constexpr std::size_t first_key_size =
            first_payload_size - prefix_size;

        const auto first_key = patterned_bytes(first_key_size, 0x15);
        const auto second_key = bytes_of("b");
        const auto second_value = bytes_of("v");

        Fragment first;
        first.header.fragment_type = Fragment::Type::FULL;
        first.header.type = ::Type::Put;
        first.header.seq_num = 1;
        first.payload.bytes = encode_logical_payload(first_key, {});
        ASSERT_TRUE(finalize_fragment(first));

        Fragment second;
        second.header.fragment_type = Fragment::Type::FULL;
        second.header.type = ::Type::Put;
        second.header.seq_num = 2;
        second.payload.bytes = encode_logical_payload(second_key, second_value);
        ASSERT_TRUE(finalize_fragment(second));

        ASSERT_TRUE(write_manual_wal(
            path_,
            wal_id,
            1,
            { std::move(first), std::move(second) }
        ));

        auto arena = make_test_arena();
        auto recovered = WAL::recover(path_, wal_id, *arena);

        ASSERT_TRUE(recovered.is_ok()) << recovered.status.message;
        EXPECT_TRUE(recovered.value.ok) << recovered.value.error;
        EXPECT_FALSE(recovered.value.had_corruption) << recovered.value.error;
        ASSERT_EQ(recovered.value.records.size(), 2u);
    }

    TEST_F(WALTest, TornFragmentedTailReturnsOnlyCompleteRecordsAndSafeOffset)
    {
        constexpr std::uint32_t wal_id = 10;

        OwnedRecord complete(
            100,
            ::Type::Put,
            bytes_of("complete"),
            bytes_of("kept")
        );
        OwnedRecord torn(
            101,
            ::Type::Put,
            bytes_of("large"),
            patterned_bytes(WAL_FILE_BLOCK_SIZE * 3u, 0x27)
        );

        WALWriter writer;
        ASSERT_STATUS_OK(writer.create(path_, wal_id, complete.record.seq_num));
        ASSERT_STATUS_OK(writer.write(complete.record));
        const std::uint64_t expected_last_good_offset = writer.offset();
        ASSERT_STATUS_OK(writer.write(torn.record));
        ASSERT_STATUS_OK(writer.close());

        const auto original_size = std::filesystem::file_size(path_);
        ASSERT_GT(original_size, expected_last_good_offset + 1u);
        std::filesystem::resize_file(path_, original_size - 1u);

        auto read_result = open_readable_file(path_);
        ASSERT_TRUE(read_result.is_ok()) << read_result.status.message;
        ASSERT_TRUE(read_result.value != nullptr);

        std::unique_ptr<ReadableFile> file = std::move(read_result.value);
        std::uint64_t offset = 0;
        auto arena = make_test_arena();

        auto recovered = WALLoader::load(*file, offset, wal_id, *arena);
        ASSERT_TRUE(recovered.is_ok()) << recovered.status.message;
        ASSERT_STATUS_OK(file->close());

        EXPECT_TRUE(recovered.value.ok);
        EXPECT_TRUE(recovered.value.had_torn_tail);
        EXPECT_FALSE(recovered.value.had_corruption);
        ASSERT_EQ(recovered.value.records.size(), 1u);
        expect_record_equals(recovered.value.records[0], complete);

        // The externally returned offset must be a logical-record boundary, not
        // the beginning of the final torn physical fragment.
        EXPECT_EQ(offset, expected_last_good_offset);
    }

    TEST_F(WALTest, PayloadBitFlipIsReportedAsCorruption)
    {
        constexpr std::uint32_t wal_id = 11;
        OwnedRecord input(
            1,
            ::Type::Put,
            bytes_of("key"),
            bytes_of("value")
        );

        WALWriter writer;
        ASSERT_STATUS_OK(writer.create(path_, wal_id, input.record.seq_num));
        ASSERT_STATUS_OK(writer.write(input.record));
        ASSERT_STATUS_OK(writer.close());

        const std::uint64_t payload_offset =
            WALFileHeader::disk_size() + Fragment::Header::disk_size();
        ASSERT_TRUE(xor_file_byte(path_, payload_offset));

        auto arena = make_test_arena();
        auto recovered = WAL::recover(path_, wal_id, *arena);

        ASSERT_TRUE(recovered.is_ok()) << recovered.status.message;
        EXPECT_FALSE(recovered.value.ok);
        EXPECT_FALSE(recovered.value.had_torn_tail);
        EXPECT_TRUE(recovered.value.had_corruption);
        EXPECT_TRUE(recovered.value.records.empty());
        EXPECT_FALSE(recovered.value.error.empty());
    }

    TEST_F(WALTest, CorruptedFileHeaderIsRejected)
    {
        constexpr std::uint32_t wal_id = 12;

        WALWriter writer;
        ASSERT_STATUS_OK(writer.create(path_, wal_id, 1));
        ASSERT_STATUS_OK(writer.close());

        ASSERT_TRUE(xor_file_byte(path_, 0));

        auto arena = make_test_arena();
        auto recovered = WAL::recover(path_, wal_id, *arena);

        ASSERT_FALSE(recovered.is_ok());
        EXPECT_EQ(recovered.status.code, StatusCode::BadMagic);
    }

    TEST_F(WALTest, WrongExpectedWalIdIsRejected)
    {
        WALWriter writer;
        ASSERT_STATUS_OK(writer.create(path_, 50, 1));
        ASSERT_STATUS_OK(writer.close());

        auto arena = make_test_arena();
        auto recovered = WAL::recover(path_, 51, *arena);

        ASSERT_FALSE(recovered.is_ok());
        EXPECT_EQ(recovered.status.code, StatusCode::InvalidHeader);
    }

    TEST_F(WALTest, MiddleFragmentWithoutFirstIsCorruption)
    {
        constexpr std::uint32_t wal_id = 13;
        const auto key = bytes_of("key");
        const auto value = bytes_of("value");

        Fragment fragment;
        fragment.header.fragment_type = Fragment::Type::MIDDLE;
        fragment.header.type = ::Type::Put;
        fragment.header.seq_num = 1;
        fragment.payload.bytes = encode_logical_payload(key, value);
        ASSERT_TRUE(finalize_fragment(fragment));

        ASSERT_TRUE(write_manual_wal(
            path_, wal_id, 1, { std::move(fragment) }
        ));

        auto arena = make_test_arena();
        auto recovered = WAL::recover(path_, wal_id, *arena);

        ASSERT_TRUE(recovered.is_ok()) << recovered.status.message;
        EXPECT_FALSE(recovered.value.ok);
        EXPECT_TRUE(recovered.value.had_corruption);
        EXPECT_TRUE(recovered.value.records.empty());
    }

    TEST_F(WALTest, FragmentMetadataCannotChangeInsideLogicalRecord)
    {
        constexpr std::uint32_t wal_id = 14;

        Fragment first;
        first.header.fragment_type = Fragment::Type::FIRST;
        first.header.type = ::Type::Put;
        first.header.seq_num = 20;
        first.payload.bytes = { std::byte{0x01}, std::byte{0x02} };
        ASSERT_TRUE(finalize_fragment(first));

        Fragment last;
        last.header.fragment_type = Fragment::Type::LAST;
        last.header.type = ::Type::Put;
        last.header.seq_num = 21; // Deliberate mismatch.
        last.payload.bytes = { std::byte{0x03}, std::byte{0x04} };
        ASSERT_TRUE(finalize_fragment(last));

        ASSERT_TRUE(write_manual_wal(
            path_, wal_id, 20, { std::move(first), std::move(last) }
        ));

        auto arena = make_test_arena();
        auto recovered = WAL::recover(path_, wal_id, *arena);

        ASSERT_TRUE(recovered.is_ok()) << recovered.status.message;
        EXPECT_FALSE(recovered.value.ok);
        EXPECT_TRUE(recovered.value.had_corruption);
        EXPECT_TRUE(recovered.value.records.empty());
    }

    TEST_F(WALTest, ValidFragmentCrcDoesNotHideMalformedLogicalLengths)
    {
        constexpr std::uint32_t wal_id = 15;

        std::vector<std::byte> malformed;
        append_u32_le(malformed, 100); // Claims 100 key bytes.
        append_u32_le(malformed, 0);
        malformed.push_back(std::byte{ 0x41 }); // Only one byte is present.

        Fragment fragment;
        fragment.header.fragment_type = Fragment::Type::FULL;
        fragment.header.type = ::Type::Put;
        fragment.header.seq_num = 1;
        fragment.payload.bytes = std::move(malformed);
        ASSERT_TRUE(finalize_fragment(fragment));

        ASSERT_TRUE(write_manual_wal(
            path_, wal_id, 1, { std::move(fragment) }
        ));

        auto arena = make_test_arena();
        auto recovered = WAL::recover(path_, wal_id, *arena);

        ASSERT_TRUE(recovered.is_ok()) << recovered.status.message;
        EXPECT_FALSE(recovered.value.ok);
        EXPECT_TRUE(recovered.value.had_corruption);
        EXPECT_TRUE(recovered.value.records.empty());
    }

    TEST_F(WALTest, InvalidRecordArgumentsAreRejectedBeforeWriting)
    {
        constexpr std::uint32_t wal_id = 16;

        WALWriter writer;
        ASSERT_STATUS_OK(writer.create(path_, wal_id, 1));
        const std::uint64_t initial_offset = writer.offset();

        InternalRecord null_key{};
        null_key.seq_num = 1;
        null_key.type = ::Type::Put;
        null_key.key_entry.data = nullptr;
        null_key.key_entry.size = 1;

        EXPECT_STATUS_CODE(
            writer.write(null_key),
            StatusCode::InvalidArgument
        );
        EXPECT_EQ(writer.offset(), initial_offset);

        InternalRecord invalid_type{};
        invalid_type.seq_num = 2;
        invalid_type.type = static_cast<::Type>(0xffu);

        EXPECT_STATUS_CODE(
            writer.write(invalid_type),
            StatusCode::InvalidArgument
        );
        EXPECT_EQ(writer.offset(), initial_offset);

        ASSERT_STATUS_OK(writer.close());
    }

    TEST_F(WALTest, WriterOperationsRequireOpenWriterAndCloseIsIdempotent)
    {
        WALWriter writer;
        OwnedRecord input(
            1,
            ::Type::Put,
            bytes_of("k"),
            bytes_of("v")
        );

        EXPECT_STATUS_CODE(
            writer.write(input.record),
            StatusCode::FailedPrecondition
        );
        EXPECT_STATUS_CODE(writer.sync(), StatusCode::FailedPrecondition);

        ASSERT_STATUS_OK(writer.close());
        ASSERT_STATUS_OK(writer.close());
    }

    TEST_F(WALTest, StreamingLoaderReturnsOneRecordPerCallAndEofIsIdempotent)
    {
        constexpr std::uint32_t wal_id = 17;

        OwnedRecord first(
            1,
            ::Type::Put,
            bytes_of("first"),
            bytes_of("one")
        );
        OwnedRecord second(
            2,
            ::Type::Put,
            bytes_of("second"),
            patterned_bytes(WAL_FILE_BLOCK_SIZE + 19u, 0x50)
        );

        WALWriter writer;
        ASSERT_STATUS_OK(writer.create(path_, wal_id, first.record.seq_num));
        ASSERT_STATUS_OK(writer.write(first.record));
        const std::uint64_t first_end = writer.offset();
        ASSERT_STATUS_OK(writer.write(second.record));
        const std::uint64_t second_end = writer.offset();
        ASSERT_STATUS_OK(writer.close());

        auto arena = make_test_arena();
        WALStreamingLoader loader(path_, *arena);
        ASSERT_STATUS_OK(loader.open());

        std::uint64_t offset = 0;

        ASSERT_STATUS_OK(loader.load_next(offset, wal_id));
        ASSERT_TRUE(loader.result().header.has_value());
        ASSERT_TRUE(loader.result().logical_record.has_value());
        expect_record_equals(*loader.result().logical_record, first);
        EXPECT_EQ(offset, first_end);
        EXPECT_EQ(loader.result().last_good_offset, first_end);

        ASSERT_STATUS_OK(loader.load_next(offset, wal_id));
        ASSERT_TRUE(loader.result().logical_record.has_value());
        expect_record_equals(*loader.result().logical_record, second);
        EXPECT_EQ(offset, second_end);
        EXPECT_EQ(loader.result().last_good_offset, second_end);

        ASSERT_STATUS_OK(loader.load_next(offset, wal_id));
        EXPECT_TRUE(loader.result().reached_eof);
        EXPECT_FALSE(loader.result().logical_record.has_value());
        EXPECT_EQ(loader.result().last_good_offset, second_end);

        const std::uint64_t eof_offset = offset;
        ASSERT_STATUS_OK(loader.load_next(offset, wal_id));
        EXPECT_TRUE(loader.result().reached_eof);
        EXPECT_EQ(offset, eof_offset);
        EXPECT_EQ(loader.result().last_good_offset, second_end);
    }

    TEST_F(WALTest, StreamingLoaderKeepsLastGoodOffsetAcrossTornTail)
    {
        constexpr std::uint32_t wal_id = 18;

        OwnedRecord complete(
            1,
            ::Type::Put,
            bytes_of("complete"),
            bytes_of("record")
        );
        OwnedRecord torn(
            2,
            ::Type::Put,
            bytes_of("torn"),
            patterned_bytes(WAL_FILE_BLOCK_SIZE * 2u + 500u, 0x70)
        );

        WALWriter writer;
        ASSERT_STATUS_OK(writer.create(path_, wal_id, 1));
        ASSERT_STATUS_OK(writer.write(complete.record));
        const std::uint64_t expected_last_good = writer.offset();
        ASSERT_STATUS_OK(writer.write(torn.record));
        ASSERT_STATUS_OK(writer.close());

        const auto original_size = std::filesystem::file_size(path_);
        std::filesystem::resize_file(path_, original_size - 1u);

        auto arena = make_test_arena();
        WALStreamingLoader loader(path_, *arena);
        ASSERT_STATUS_OK(loader.open());

        std::uint64_t offset = 0;

        ASSERT_STATUS_OK(loader.load_next(offset, wal_id));
        ASSERT_TRUE(loader.result().logical_record.has_value());
        expect_record_equals(*loader.result().logical_record, complete);
        EXPECT_EQ(loader.result().last_good_offset, expected_last_good);

        ASSERT_STATUS_OK(loader.load_next(offset, wal_id));
        EXPECT_TRUE(loader.result().ok);
        EXPECT_TRUE(loader.result().had_torn_tail);
        EXPECT_FALSE(loader.result().had_corruption);
        EXPECT_FALSE(loader.result().logical_record.has_value());
        EXPECT_EQ(loader.result().last_good_offset, expected_last_good);
        EXPECT_EQ(offset, expected_last_good);

        // Terminal torn-tail state should be stable and should not read again.
        ASSERT_STATUS_OK(loader.load_next(offset, wal_id));
        EXPECT_TRUE(loader.result().had_torn_tail);
        EXPECT_EQ(loader.result().last_good_offset, expected_last_good);
        EXPECT_EQ(offset, expected_last_good);
    }

    TEST_F(WALTest, StreamingLoaderMustBeOpened)
    {
        auto arena = make_test_arena();
        WALStreamingLoader loader(path_, *arena);

        std::uint64_t offset = 0;
        EXPECT_STATUS_CODE(
            loader.load_next(offset, 1),
            StatusCode::InvariantViolation
        );
    }

} // namespace