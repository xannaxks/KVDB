#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <zlib.h>

#include "sstable_entities/data_section_view.h"

namespace
{
    using namespace SSTableEntities;

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
            ++read_calls_;
            requested_bytes_ += size;
            bytes_read = 0;

            if (fail_next_read_) {
                fail_next_read_ = false;
                return Status{
                    StatusCode::ReadFailed,
                    "injected read failure"
                };
            }

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

        void fail_next_read() noexcept { fail_next_read_ = true; }

        void flip(std::size_t offset)
        {
            bytes_.at(offset) ^= std::byte{ 0x01 };
        }

        [[nodiscard]] std::size_t read_calls() const noexcept
        {
            return read_calls_;
        }

        [[nodiscard]] std::size_t requested_bytes() const noexcept
        {
            return requested_bytes_;
        }

    private:
        std::vector<std::byte> bytes_;
        std::size_t read_calls_ = 0;
        std::size_t requested_bytes_ = 0;
        bool fail_next_read_ = false;
#ifndef _WIN32
        int descriptor_ = -1;
#endif
    };

    struct EncodedRecord
    {
        std::string key;
        std::string value;
        ::Type type = ::Type::Put;
        std::uint64_t seq_num = 0;
        std::uint32_t flags = 0;
        std::uint32_t reserved = 0;
    };

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

    [[nodiscard]] std::vector<std::byte> encode_payload(
        const std::vector<EncodedRecord>& records
    )
    {
        std::vector<std::byte> result;
        for (const EncodedRecord& record : records) {
            append_u32_le(
                result,
                static_cast<std::uint32_t>(record.key.size())
            );
            append_u32_le(
                result,
                static_cast<std::uint32_t>(record.value.size())
            );
            result.push_back(static_cast<std::byte>(record.type));
            append_u32_le(result, record.flags);
            append_u32_le(result, record.reserved);
            append_u64_le(result, record.seq_num);

            for (char value : record.key) {
                result.push_back(static_cast<std::byte>(value));
            }
            for (char value : record.value) {
                result.push_back(static_cast<std::byte>(value));
            }
        }
        return result;
    }

    [[nodiscard]] std::uint32_t payload_crc(
        std::span<const std::byte> payload
    )
    {
        std::uint32_t crc = ::crc32(0L, Z_NULL, 0);
        if (!payload.empty()) {
            crc = ::crc32(
                crc,
                reinterpret_cast<const Bytef*>(payload.data()),
                static_cast<uInt>(payload.size())
            );
        }
        return crc;
    }

    void write_u32_le_at(
        std::vector<std::byte>& bytes,
        std::size_t offset,
        std::uint32_t value
    )
    {
        for (std::size_t i = 0; i < sizeof(value); ++i) {
            bytes.at(offset + i) = static_cast<std::byte>(
                (value >> (i * 8u)) & 0xFFu
                );
        }
    }

    void encode_block(
        std::vector<std::byte>& file,
        std::uint64_t block_offset,
        const std::vector<std::byte>& payload,
        BlockType type = BlockType::Data,
        std::optional<std::uint32_t> forced_crc = std::nullopt,
        std::optional<std::uint32_t> forced_payload_size = std::nullopt
    )
    {
        const std::size_t offset = static_cast<std::size_t>(block_offset);
        const std::size_t required = offset + BLOCK_SIZE;
        if (file.size() < required) {
            file.resize(required, std::byte{ 0 });
        }

        file.at(offset) = static_cast<std::byte>(type);
        write_u32_le_at(
            file,
            offset + 1,
            forced_payload_size.value_or(
                static_cast<std::uint32_t>(payload.size())
            )
        );
        write_u32_le_at(
            file,
            offset + 5,
            forced_crc.value_or(payload_crc(payload))
        );

        std::copy(
            payload.begin(),
            payload.end(),
            file.begin() + static_cast<std::ptrdiff_t>(
                offset + DataSection::Header::disk_size()
                )
        );
    }

    [[nodiscard]] std::vector<std::byte> one_block_file()
    {
        std::vector<std::byte> file;
        encode_block(
            file,
            0,
            encode_payload({
                EncodedRecord{"alpha", "new", ::Type::Put, 9},
                EncodedRecord{"alpha", "old", ::Type::Put, 4},
                EncodedRecord{"omega", "", ::Type::Tombstone, 3}
                })
        );
        return file;
    }

    [[nodiscard]] Result<DataSectionView> load_one(
        MemoryReadableFile& file,
        std::uint64_t& offset
    )
    {
        return DataSectionView::load(
            file,
            offset,
            0,
            1,
            BLOCK_SIZE
        );
    }

    [[nodiscard]] std::uint64_t used_bytes_or_zero(const Arena& arena)
    {
        Result<std::uint64_t> used = arena.get_used_bytes();
        return used.is_ok() ? used.value : 0;
    }
}

static_assert(!std::is_nothrow_copy_constructible_v<DataSectionView>);
static_assert(std::is_nothrow_move_constructible_v<DataSectionView>);

TEST(DataSectionViewTest, LoadReadsOnlyBlockHeaders)
{
    MemoryReadableFile file(one_block_file());
    std::uint64_t offset = 777;

    Result<DataSectionView> result = load_one(file, offset);

    ASSERT_TRUE(result.is_ok()) << result.status.message;
    EXPECT_EQ(offset, BLOCK_SIZE);
    ASSERT_EQ(result.value.data_blocks.size(), 1u);
    EXPECT_EQ(
        result.value.data_blocks[0].validation_state(),
        DataSectionView::DataBlock::ValidationState::Unvalidated
    );
    EXPECT_EQ(result.value.data_blocks[0].record_count(), 0u);
    EXPECT_EQ(file.requested_bytes(), DataSection::Header::disk_size());
}

TEST(DataSectionViewTest, ZeroFirstOffsetIsAbsoluteNotASentinel)
{
    MemoryReadableFile file(one_block_file());
    std::uint64_t offset = 1234;

    Result<DataSectionView> result = DataSectionView::load(
        file,
        offset,
        0,
        1,
        BLOCK_SIZE
    );

    ASSERT_TRUE(result.is_ok()) << result.status.message;
    EXPECT_EQ(result.value.data_blocks.front().header_view.header_offset, 0u);
    EXPECT_EQ(offset, BLOCK_SIZE);
}

TEST(DataSectionViewTest, RejectsPartialFinalPhysicalBlock)
{
    std::vector<std::byte> bytes(1, std::byte{ 0 });
    MemoryReadableFile file(std::move(bytes));
    std::uint64_t offset = 91;

    Result<DataSectionView> result = DataSectionView::load(
        file,
        offset,
        0,
        1
    );

    ASSERT_FALSE(result.is_ok());
    EXPECT_EQ(result.status.code, StatusCode::InvalidSectionSize);
    EXPECT_EQ(offset, 91u);
    EXPECT_EQ(file.read_calls(), 0u);
}

TEST(DataSectionViewTest, RejectsTrustedEndThatDisagreesWithBlockCount)
{
    std::vector<std::byte> bytes(3 * BLOCK_SIZE, std::byte{ 0 });
    MemoryReadableFile file(std::move(bytes));
    std::uint64_t offset = 42;

    Result<DataSectionView> result = DataSectionView::load(
        file,
        offset,
        BLOCK_SIZE,
        1,
        3 * BLOCK_SIZE
    );

    ASSERT_FALSE(result.is_ok());
    EXPECT_EQ(result.status.code, StatusCode::InvalidSectionSize);
    EXPECT_EQ(offset, 42u);
}

TEST(DataSectionViewTest, FailedHeaderDiscoveryRestoresCallerOffset)
{
    std::vector<std::byte> bytes(BLOCK_SIZE, std::byte{ 0 });
    encode_block(
        bytes,
        0,
        encode_payload({ EncodedRecord{"a", "b", ::Type::Put, 1} }),
        BlockType::Index
    );
    MemoryReadableFile file(std::move(bytes));
    std::uint64_t offset = 51;

    Result<DataSectionView> result = load_one(file, offset);

    ASSERT_FALSE(result.is_ok());
    EXPECT_EQ(result.status.code, StatusCode::InvalidBlockType);
    EXPECT_EQ(offset, 51u);
}

TEST(DataSectionViewTest, ReportsThreeDifferentSizeMeanings)
{
    std::vector<std::byte> bytes;
    const std::vector<std::byte> first = encode_payload({
        EncodedRecord{"a", "1", ::Type::Put, 4}
        });
    const std::vector<std::byte> second = encode_payload({
        EncodedRecord{"b", "222", ::Type::Put, 3},
        EncodedRecord{"c", "", ::Type::Tombstone, 2}
        });
    encode_block(bytes, 0, first);
    encode_block(bytes, BLOCK_SIZE, second);

    MemoryReadableFile file(std::move(bytes));
    std::uint64_t offset = 0;
    Result<DataSectionView> loaded = DataSectionView::load(
        file,
        offset,
        0,
        2,
        2 * BLOCK_SIZE
    );
    ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;

    Result<std::uint64_t> logical = loaded.value.logical_size();
    Result<std::uint64_t> physical = loaded.value.physical_span();
    Result<std::uint64_t> reserved = loaded.value.reserved_span();

    ASSERT_TRUE(logical.is_ok());
    ASSERT_TRUE(physical.is_ok());
    ASSERT_TRUE(reserved.is_ok());
    EXPECT_EQ(
        logical.value,
        2 * DataSection::Header::disk_size() + first.size() + second.size()
    );
    EXPECT_EQ(
        physical.value,
        BLOCK_SIZE + DataSection::Header::disk_size() + second.size()
    );
    EXPECT_EQ(reserved.value, 2 * BLOCK_SIZE);
}

TEST(DataSectionViewTest, EmptySectionHasZeroSizesAndReadsNothing)
{
    MemoryReadableFile file(std::vector<std::byte>(BLOCK_SIZE, std::byte{ 0 }));
    std::uint64_t offset = 19;

    Result<DataSectionView> loaded = DataSectionView::load(
        file,
        offset,
        123,
        0,
        BLOCK_SIZE
    );

    ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;
    EXPECT_EQ(offset, 123u);
    EXPECT_TRUE(loaded.value.data_blocks.empty());
    EXPECT_EQ(loaded.value.logical_size().value, 0u);
    EXPECT_EQ(loaded.value.physical_span().value, 0u);
    EXPECT_EQ(loaded.value.reserved_span().value, 0u);
    EXPECT_EQ(file.read_calls(), 0u);
}

TEST(DataSectionViewTest, FirstValidationChecksCrcAndCachesSuccess)
{
    MemoryReadableFile file(one_block_file());
    std::uint64_t offset = 0;
    Result<DataSectionView> loaded = load_one(file, offset);
    ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;

    const std::size_t after_headers = file.requested_bytes();
    Status first = loaded.value.validate_block(file, 0);
    ASSERT_TRUE(first.is_ok()) << first.message;
    EXPECT_GT(file.requested_bytes(), after_headers);
    EXPECT_EQ(
        loaded.value.data_blocks[0].validation_state(),
        DataSectionView::DataBlock::ValidationState::Valid
    );
    EXPECT_EQ(loaded.value.data_blocks[0].record_count(), 3u);

    const std::size_t after_first_validation = file.requested_bytes();
    Status second = loaded.value.validate_block(file, 0);
    EXPECT_TRUE(second.is_ok()) << second.message;
    EXPECT_EQ(file.requested_bytes(), after_first_validation);
}

TEST(DataSectionViewTest, ChecksumFailureIsCachedWithoutRereading)
{
    std::vector<std::byte> bytes = one_block_file();
    bytes.at(DataSection::Header::disk_size()) ^= std::byte{ 0x01 };
    MemoryReadableFile file(std::move(bytes));
    std::uint64_t offset = 0;
    Result<DataSectionView> loaded = load_one(file, offset);
    ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;

    Status first = loaded.value.validate_block(file, 0);
    ASSERT_FALSE(first.is_ok());
    EXPECT_EQ(first.code, StatusCode::ChecksumMismatch);
    EXPECT_EQ(
        loaded.value.data_blocks[0].validation_state(),
        DataSectionView::DataBlock::ValidationState::Corrupt
    );

    const std::size_t after_failure = file.requested_bytes();
    Status second = loaded.value.validate_block(file, 0);
    EXPECT_FALSE(second.is_ok());
    EXPECT_EQ(second.code, StatusCode::ChecksumMismatch);
    EXPECT_EQ(file.requested_bytes(), after_failure);
}

TEST(DataSectionViewTest, TransientReadFailureIsNotCached)
{
    MemoryReadableFile file(one_block_file());
    std::uint64_t offset = 0;
    Result<DataSectionView> loaded = load_one(file, offset);
    ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;

    file.fail_next_read();
    Status failed = loaded.value.validate_block(file, 0);
    ASSERT_FALSE(failed.is_ok());
    EXPECT_EQ(failed.code, StatusCode::ReadFailed);
    EXPECT_EQ(
        loaded.value.data_blocks[0].validation_state(),
        DataSectionView::DataBlock::ValidationState::Unvalidated
    );

    Status retried = loaded.value.validate_block(file, 0);
    EXPECT_TRUE(retried.is_ok()) << retried.message;
}

TEST(DataSectionViewTest, ValidCrcDoesNotHideMalformedRecordLength)
{
    std::vector<std::byte> payload = encode_payload({
        EncodedRecord{"a", "b", ::Type::Put, 1}
        });
    write_u32_le_at(payload, 0, 4000);

    std::vector<std::byte> bytes;
    encode_block(bytes, 0, payload);
    MemoryReadableFile file(std::move(bytes));
    std::uint64_t offset = 0;
    Result<DataSectionView> loaded = load_one(file, offset);
    ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;

    Status status = loaded.value.validate_block(file, 0);
    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::Corruption);
}

TEST(DataSectionViewTest, RejectsInvalidRecordTypeWithValidCrc)
{
    std::vector<std::byte> payload = encode_payload({
        EncodedRecord{"a", "b", ::Type::Put, 1}
        });
    payload.at(8) = std::byte{ 0xFF };

    std::vector<std::byte> bytes;
    encode_block(bytes, 0, payload);
    MemoryReadableFile file(std::move(bytes));
    std::uint64_t offset = 0;
    Result<DataSectionView> loaded = load_one(file, offset);
    ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;

    Status status = loaded.value.validate_block(file, 0);
    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::Corruption);
}

TEST(DataSectionViewTest, RejectsNonzeroFlagsAndReservedWithValidCrc)
{
    std::vector<std::byte> bytes;
    encode_block(
        bytes,
        0,
        encode_payload({
            EncodedRecord{"a", "b", ::Type::Put, 1, 1, 0}
            })
    );
    MemoryReadableFile file(std::move(bytes));
    std::uint64_t offset = 0;
    Result<DataSectionView> loaded = load_one(file, offset);
    ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;

    Status status = loaded.value.validate_block(file, 0);
    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::Corruption);
}

TEST(DataSectionViewTest, RejectsIncorrectRecordOrderingWithValidCrc)
{
    std::vector<std::byte> bytes;
    encode_block(
        bytes,
        0,
        encode_payload({
            EncodedRecord{"z", "1", ::Type::Put, 9},
            EncodedRecord{"a", "2", ::Type::Put, 8}
            })
    );
    MemoryReadableFile file(std::move(bytes));
    std::uint64_t offset = 0;
    Result<DataSectionView> loaded = load_one(file, offset);
    ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;

    Status status = loaded.value.validate_block(file, 0);
    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::Corruption);
}

TEST(DataSectionViewTest, RejectsEqualKeysWithoutStrictlyDescendingSequence)
{
    std::vector<std::byte> bytes;
    encode_block(
        bytes,
        0,
        encode_payload({
            EncodedRecord{"same", "1", ::Type::Put, 7},
            EncodedRecord{"same", "2", ::Type::Put, 7}
            })
    );
    MemoryReadableFile file(std::move(bytes));
    std::uint64_t offset = 0;
    Result<DataSectionView> loaded = load_one(file, offset);
    ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;

    Status status = loaded.value.validate_block(file, 0);
    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::Corruption);
}

TEST(DataSectionViewTest, ReadRecordValidatesThenCopiesOnlyRequestedRecord)
{
    MemoryReadableFile file(one_block_file());
    std::uint64_t offset = 0;
    Result<DataSectionView> loaded = load_one(file, offset);
    ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;

    Arena arena;
    Result<InternalRecord> record = loaded.value.read_record(
        file,
        0,
        1,
        arena
    );

    ASSERT_TRUE(record.is_ok()) << record.status.message;
    EXPECT_EQ(record.value.type, ::Type::Put);
    EXPECT_EQ(record.value.seq_num, 4u);
    EXPECT_EQ(record.value.key_entry.size, 5u);
    EXPECT_EQ(record.value.value_entry.size, 3u);
    EXPECT_EQ(
        std::string_view(
            static_cast<const char*>(record.value.key_entry.data),
            record.value.key_entry.size
        ),
        "alpha"
    );
    EXPECT_EQ(
        std::string_view(
            static_cast<const char*>(record.value.value_entry.data),
            record.value.value_entry.size
        ),
        "old"
    );
}

TEST(DataSectionViewTest, ReadRecordRollsBackArenaAfterReadFailure)
{
    MemoryReadableFile file(one_block_file());
    std::uint64_t offset = 0;
    Result<DataSectionView> loaded = load_one(file, offset);
    ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;
    ASSERT_TRUE(loaded.value.validate_block(file, 0).is_ok());

    Arena arena;
    const std::uint64_t before = used_bytes_or_zero(arena);
    file.fail_next_read();

    Result<InternalRecord> record = loaded.value.read_record(
        file,
        0,
        0,
        arena
    );

    ASSERT_FALSE(record.is_ok());
    EXPECT_EQ(record.status.code, StatusCode::ReadFailed);
    EXPECT_EQ(used_bytes_or_zero(arena), before);
}

TEST(DataSectionViewTest, RejectsBlockAndRecordIndexesOutsideView)
{
    MemoryReadableFile file(one_block_file());
    std::uint64_t offset = 0;
    Result<DataSectionView> loaded = load_one(file, offset);
    ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;

    EXPECT_EQ(
        loaded.value.validate_block(file, 1).code,
        StatusCode::InvalidArgument
    );

    Arena arena;
    Result<InternalRecord> bad_block = loaded.value.read_record(
        file,
        1,
        0,
        arena
    );
    ASSERT_FALSE(bad_block.is_ok());
    EXPECT_EQ(bad_block.status.code, StatusCode::InvalidArgument);

    Result<InternalRecord> bad_record = loaded.value.read_record(
        file,
        0,
        99,
        arena
    );
    ASSERT_FALSE(bad_record.is_ok());
    EXPECT_EQ(bad_record.status.code, StatusCode::InvalidArgument);
}

TEST(DataSectionViewTest, RejectsPayloadSizeAboveBlockCapacityEagerly)
{
    std::vector<std::byte> bytes(BLOCK_SIZE, std::byte{ 0 });
    encode_block(
        bytes,
        0,
        {},
        BlockType::Data,
        0,
        static_cast<std::uint32_t>(BLOCK_SIZE)
    );
    MemoryReadableFile file(std::move(bytes));
    std::uint64_t offset = 9;

    Result<DataSectionView> loaded = load_one(file, offset);

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::InvalidPayloadSize);
    EXPECT_EQ(offset, 9u);
}

TEST(DataSectionViewTest, FindFirstRecordReturnsNewestDuplicate)
{
    MemoryReadableFile file(one_block_file());
    std::uint64_t offset = 0;
    Result<DataSectionView> loaded = load_one(file, offset);
    ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;

    std::string searched_key = "alpha";
    ArenaEntry key{};
    key.data = searched_key.data();
    key.size = searched_key.size();

    Result<std::optional<std::size_t>> found =
        loaded.value.find_first_record(file, 0, key);

    ASSERT_TRUE(found.is_ok()) << found.status.message;
    ASSERT_TRUE(found.value.has_value());
    EXPECT_EQ(*found.value, 0u);

    Arena arena;
    Result<InternalRecord> record = loaded.value.read_record(
        file,
        0,
        *found.value,
        arena
    );

    ASSERT_TRUE(record.is_ok()) << record.status.message;
    EXPECT_EQ(record.value.seq_num, 9u);
    EXPECT_EQ(
        std::string_view(
            static_cast<const char*>(record.value.value_entry.data),
            record.value.value_entry.size
        ),
        "new"
    );
}

TEST(DataSectionViewTest, FindFirstRecordCanFindLaterTombstone)
{
    MemoryReadableFile file(one_block_file());
    std::uint64_t offset = 0;
    Result<DataSectionView> loaded = load_one(file, offset);
    ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;

    std::string searched_key = "omega";
    ArenaEntry key{};
    key.data = searched_key.data();
    key.size = searched_key.size();

    Result<std::optional<std::size_t>> found =
        loaded.value.find_first_record(file, 0, key);

    ASSERT_TRUE(found.is_ok()) << found.status.message;
    ASSERT_TRUE(found.value.has_value());
    EXPECT_EQ(*found.value, 2u);

    Arena arena;
    Result<InternalRecord> record = loaded.value.read_record(
        file,
        0,
        *found.value,
        arena
    );

    ASSERT_TRUE(record.is_ok()) << record.status.message;
    EXPECT_EQ(record.value.type, ::Type::Tombstone);
    EXPECT_EQ(record.value.seq_num, 3u);
    EXPECT_EQ(record.value.value_entry.size, 0u);
}

TEST(DataSectionViewTest, FindFirstRecordReturnsNulloptForMissingKeys)
{
    MemoryReadableFile file(one_block_file());
    std::uint64_t offset = 0;
    Result<DataSectionView> loaded = load_one(file, offset);
    ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;

    for (std::string searched_key : { "aardvark", "beta", "zulu" }) {
        ArenaEntry key{};
        key.data = searched_key.data();
        key.size = searched_key.size();

        Result<std::optional<std::size_t>> found =
            loaded.value.find_first_record(file, 0, key);

        ASSERT_TRUE(found.is_ok()) << found.status.message;
        EXPECT_FALSE(found.value.has_value()) << searched_key;
    }
}

TEST(DataSectionViewTest, FindFirstRecordRejectsInvalidArguments)
{
    MemoryReadableFile file(one_block_file());
    std::uint64_t offset = 0;
    Result<DataSectionView> loaded = load_one(file, offset);
    ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;

    ArenaEntry valid_key{};
    std::string searched_key = "alpha";
    valid_key.data = searched_key.data();
    valid_key.size = searched_key.size();

    Result<std::optional<std::size_t>> bad_block =
        loaded.value.find_first_record(file, 1, valid_key);

    ASSERT_FALSE(bad_block.is_ok());
    EXPECT_EQ(bad_block.status.code, StatusCode::InvalidArgument);

    ArenaEntry invalid_key{};
    invalid_key.data = nullptr;
    invalid_key.size = 1;

    const std::size_t bytes_before = file.requested_bytes();
    Result<std::optional<std::size_t>> bad_key =
        loaded.value.find_first_record(file, 0, invalid_key);

    ASSERT_FALSE(bad_key.is_ok());
    EXPECT_EQ(bad_key.status.code, StatusCode::InvalidArgument);
    EXPECT_EQ(file.requested_bytes(), bytes_before);
}

TEST(DataSectionViewTest, FindFirstRecordPropagatesSearchReadFailure)
{
    MemoryReadableFile file(one_block_file());
    std::uint64_t offset = 0;
    Result<DataSectionView> loaded = load_one(file, offset);
    ASSERT_TRUE(loaded.is_ok()) << loaded.status.message;
    ASSERT_TRUE(loaded.value.validate_block(file, 0).is_ok());

    std::string searched_key = "alpha";
    ArenaEntry key{};
    key.data = searched_key.data();
    key.size = searched_key.size();

    file.fail_next_read();
    Result<std::optional<std::size_t>> found =
        loaded.value.find_first_record(file, 0, key);

    ASSERT_FALSE(found.is_ok());
    EXPECT_EQ(found.status.code, StatusCode::ReadFailed);

    // A transient search read failure does not corrupt the cached block state.
    EXPECT_EQ(
        loaded.value.data_blocks[0].validation_state(),
        DataSectionView::DataBlock::ValidationState::Valid
    );

    Result<std::optional<std::size_t>> retried =
        loaded.value.find_first_record(file, 0, key);
    ASSERT_TRUE(retried.is_ok()) << retried.status.message;
    ASSERT_TRUE(retried.value.has_value());
    EXPECT_EQ(*retried.value, 0u);
}
