#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "arena.h"
#include "crc32_helpers.h"
#include "sstable_entities.h"
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
                    "test writable offset mismatch"
                };
            }

            const std::size_t remaining =
                bytes_written_ >= fail_after_bytes_
                ? 0
                : fail_after_bytes_ - bytes_written_;
            const std::size_t writable = std::min(size, remaining);

            if (writable > 0) {
                const std::size_t required =
                    static_cast<std::size_t>(position_) + writable;
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
                    "injected index write failure"
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
        const void* get_descriptor() const override { return nullptr; }
#else
        const int& get_descriptor() const override { return descriptor_; }
#endif

        [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept
        {
            return bytes_;
        }

        [[nodiscard]] std::uint64_t position() const noexcept
        {
            return position_;
        }

    private:
#ifndef _WIN32
        int descriptor_ = -1;
#endif
        std::uint64_t position_ = 0;
        std::size_t fail_after_bytes_ =
            std::numeric_limits<std::size_t>::max();
        std::size_t bytes_written_ = 0;
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
                return Status{
                    StatusCode::NullPointer,
                    "test reader received null buffer"
                };
            }

            if (offset > bytes_.size()) {
                return Status{
                    StatusCode::InvalidOffset,
                    "test read offset exceeds file size"
                };
            }

            bytes_read = std::min<std::size_t>(
                size,
                bytes_.size() - static_cast<std::size_t>(offset)
            );

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
        const void* get_descriptor() const override { return nullptr; }
#else
        const int& get_descriptor() const override { return descriptor_; }
#endif

    private:
#ifndef _WIN32
        int descriptor_ = -1;
#endif
        std::vector<std::byte> bytes_;
    };

    struct RawEntry
    {
        std::uint64_t data_block_offset = 0;
        std::string first_key;
        std::string last_key;
    };

    void append_u32_le(
        std::vector<std::byte>& bytes,
        std::uint32_t value
    )
    {
        for (std::size_t i = 0; i < sizeof(value); ++i) {
            bytes.push_back(static_cast<std::byte>(
                (value >> (i * 8U)) & 0xFFU
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
                (value >> (i * 8U)) & 0xFFU
                ));
        }
    }

    [[nodiscard]] std::uint32_t read_u32_le(
        const std::vector<std::byte>& bytes,
        std::size_t position
    )
    {
        return
            std::to_integer<std::uint32_t>(bytes.at(position)) |
            (std::to_integer<std::uint32_t>(
                bytes.at(position + 1)) << 8U) |
            (std::to_integer<std::uint32_t>(
                bytes.at(position + 2)) << 16U) |
            (std::to_integer<std::uint32_t>(
                bytes.at(position + 3)) << 24U);
    }

    [[nodiscard]] std::vector<std::byte> encode_index(
        const std::vector<RawEntry>& entries,
        std::uint64_t index_offset = 0
    )
    {
        std::vector<std::byte> payload;

        for (const RawEntry& entry : entries) {
            append_u64_le(payload, entry.data_block_offset);
            append_u32_le(
                payload,
                static_cast<std::uint32_t>(entry.first_key.size())
            );
            append_u32_le(
                payload,
                static_cast<std::uint32_t>(entry.last_key.size())
            );

            const auto* first = reinterpret_cast<const std::byte*>(
                entry.first_key.data()
                );
            payload.insert(
                payload.end(),
                first,
                first + entry.first_key.size()
            );

            const auto* last = reinterpret_cast<const std::byte*>(
                entry.last_key.data()
                );
            payload.insert(
                payload.end(),
                last,
                last + entry.last_key.size()
            );
        }

        std::vector<std::byte> result(
            static_cast<std::size_t>(index_offset),
            std::byte{ 0 }
        );
        result.push_back(static_cast<std::byte>(BlockType::Index));
        append_u32_le(
            result,
            static_cast<std::uint32_t>(payload.size())
        );
        append_u32_le(
            result,
            crc32_of(payload.data(), payload.size())
        );
        result.insert(result.end(), payload.begin(), payload.end());
        return result;
    }

    [[nodiscard]] std::string_view bytes_view(
        const void* pointer,
        std::uint32_t size
    )
    {
        return std::string_view(
            static_cast<const char*>(pointer),
            size
        );
    }

    [[nodiscard]] IndexSection make_three_range_index()
    {
        static const std::string a = "a";
        static const std::string k = "k";
        static const std::string z = "z";

        IndexSection index;
        EXPECT_TRUE(index.add_index(
            0,
            1,
            1,
            a.data(),
            k.data()
        ).is_ok());
        EXPECT_TRUE(index.add_index(
            BLOCK_SIZE,
            1,
            1,
            k.data(),
            k.data()
        ).is_ok());
        EXPECT_TRUE(index.add_index(
            2ULL * BLOCK_SIZE,
            1,
            1,
            k.data(),
            z.data()
        ).is_ok());
        return index;
    }
}

TEST(IndexSectionLayoutTest, UsesStableEncodedWidths)
{
    EXPECT_EQ(IndexSection::Header::disk_size(), 9u);
    EXPECT_EQ(IndexSection::Payload::fixed_disk_size(), 16u);
    EXPECT_EQ(IndexSection::fixed_disk_size(), 25u);
}

TEST(IndexSectionTypeTraitsTest, VectorOwnerCopyIsNotNoexcept)
{
    EXPECT_FALSE(std::is_nothrow_copy_constructible_v<IndexSection>);
    EXPECT_TRUE(std::is_nothrow_move_constructible_v<IndexSection>);
}

TEST(IndexSectionTest, EmptyIndexIsCanonical)
{
    IndexSection index;

    ASSERT_TRUE(index.validate().is_ok());
    EXPECT_TRUE(index.payloads.empty());
    EXPECT_EQ(index.header.payload_size, 0u);
    EXPECT_EQ(index.header.crc32, crc32_of(nullptr, 0));
    EXPECT_EQ(index.disk_size(), IndexSection::Header::disk_size());
}

TEST(IndexSectionTest, AddIndexRejectsInvalidPointersWithoutMutation)
{
    IndexSection index;
    const IndexSection::Header original_header = index.header;

    const Status status = index.add_index(
        0,
        1,
        0,
        nullptr,
        nullptr
    );

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::NullPointer);
    EXPECT_TRUE(index.payloads.empty());
    EXPECT_EQ(index.header.payload_size, original_header.payload_size);
    EXPECT_EQ(index.header.crc32, original_header.crc32);
}

TEST(IndexSectionTest, AddIndexRejectsUnalignedDataOffset)
{
    const std::string key = "key";
    IndexSection index;

    const Status status = index.add_index(
        1,
        3,
        3,
        key.data(),
        key.data()
    );

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::InvalidSectionOffset);
    EXPECT_TRUE(index.payloads.empty());
}

TEST(IndexSectionTest, AddIndexRejectsReversedRange)
{
    const std::string first = "z";
    const std::string last = "a";
    IndexSection index;

    const Status status = index.add_index(
        0,
        1,
        1,
        first.data(),
        last.data()
    );

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::InvalidArgument);
    EXPECT_TRUE(index.payloads.empty());
}

TEST(IndexSectionTest, RequiresConsecutiveDataBlockOffsets)
{
    const std::string a = "a";
    const std::string b = "b";
    IndexSection index;

    ASSERT_TRUE(index.add_index(
        0,
        1,
        1,
        a.data(),
        a.data()
    ).is_ok());

    const IndexSection::Header before = index.header;
    const Status status = index.add_index(
        2ULL * BLOCK_SIZE,
        1,
        1,
        b.data(),
        b.data()
    );

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::InvalidSectionOffset);
    EXPECT_EQ(index.payloads.size(), 1u);
    EXPECT_EQ(index.header.payload_size, before.payload_size);
    EXPECT_EQ(index.header.crc32, before.crc32);
}

TEST(IndexSectionTest, RejectsRangesOutOfGlobalOrder)
{
    const std::string m = "m";
    const std::string z = "z";
    const std::string a = "a";
    IndexSection index;

    ASSERT_TRUE(index.add_index(
        0,
        1,
        1,
        m.data(),
        z.data()
    ).is_ok());

    const Status status = index.add_index(
        BLOCK_SIZE,
        1,
        1,
        a.data(),
        a.data()
    );

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::OverlappingKeys);
    EXPECT_EQ(index.payloads.size(), 1u);
}

TEST(IndexSectionTest, AllowsSharedBoundaryForOneKeyAcrossBlocks)
{
    IndexSection index = make_three_range_index();

    const std::string key = "k";
    const auto range = index.find_candidate_range(
        key.data(),
        static_cast<std::uint32_t>(key.size())
    );

    ASSERT_TRUE(range.is_ok());
    EXPECT_EQ(range.value.first, 0u);
    EXPECT_EQ(range.value.last_exclusive, 3u);
    EXPECT_EQ(range.value.size(), 3u);

    const auto first = index.find_first_candidate(
        key.data(),
        static_cast<std::uint32_t>(key.size())
    );
    ASSERT_TRUE(first.is_ok());
    EXPECT_EQ(first.value, 0u);
}

TEST(IndexSectionTest, CandidateSearchReturnsCorrectLaterBlock)
{
    IndexSection index = make_three_range_index();
    const std::string key = "m";

    const auto candidate = index.find_first_candidate(
        key.data(),
        static_cast<std::uint32_t>(key.size())
    );

    ASSERT_TRUE(candidate.is_ok());
    EXPECT_EQ(candidate.value, 2u);
}

TEST(IndexSectionTest, CandidateSearchReturnsNotFound)
{
    IndexSection index = make_three_range_index();
    const std::string key = "zz";

    const auto candidate = index.find_candidate_range(
        key.data(),
        static_cast<std::uint32_t>(key.size())
    );

    ASSERT_FALSE(candidate.is_ok());
    EXPECT_EQ(candidate.status.code, StatusCode::NotFound);
}

TEST(IndexSectionTest, CandidateSearchRejectsNullNonemptyKey)
{
    IndexSection index = make_three_range_index();

    const auto candidate = index.find_candidate_range(nullptr, 1);

    ASSERT_FALSE(candidate.is_ok());
    EXPECT_EQ(candidate.status.code, StatusCode::NullPointer);
}

TEST(IndexSectionTest, SingleBlockCapacityFailureIsTransactional)
{
    const std::string large_first(2000, 'a');
    const std::string large_last(2000, 'a');
    const std::string z = "z";

    IndexSection index;
    ASSERT_TRUE(index.add_index(
        0,
        static_cast<std::uint32_t>(large_first.size()),
        static_cast<std::uint32_t>(large_last.size()),
        large_first.data(),
        large_last.data()
    ).is_ok());

    ASSERT_TRUE(index.add_index(
        BLOCK_SIZE,
        1,
        1,
        z.data(),
        z.data()
    ).is_ok());
    ASSERT_TRUE(index.add_index(
        2ULL * BLOCK_SIZE,
        1,
        1,
        z.data(),
        z.data()
    ).is_ok());
    ASSERT_TRUE(index.add_index(
        3ULL * BLOCK_SIZE,
        1,
        1,
        z.data(),
        z.data()
    ).is_ok());

    const std::size_t before_count = index.payloads.size();
    const IndexSection::Header before_header = index.header;

    const Status status = index.add_index(
        4ULL * BLOCK_SIZE,
        1,
        1,
        z.data(),
        z.data()
    );

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::InvalidSectionSize);
    EXPECT_EQ(index.payloads.size(), before_count);
    EXPECT_EQ(index.header.payload_size, before_header.payload_size);
    EXPECT_EQ(index.header.crc32, before_header.crc32);
}

TEST(IndexSectionWriteTest, RebuildsStaleHeaderAndUsesLittleEndian)
{
    const std::string first = "alpha";
    const std::string last = "omega";

    IndexSection index;
    ASSERT_TRUE(index.add_index(
        0,
        static_cast<std::uint32_t>(first.size()),
        static_cast<std::uint32_t>(last.size()),
        first.data(),
        last.data()
    ).is_ok());

    index.header.type = BlockType::Data;
    index.header.payload_size = 123;
    index.header.crc32 = 456;

    MemoryWritableFile writer(100);
    std::uint64_t offset = 100;
    std::uint64_t index_offset = 777;

    ASSERT_TRUE(index.write(writer, offset, index_offset).is_ok());

    EXPECT_EQ(index_offset, BLOCK_SIZE);
    EXPECT_EQ(
        std::to_integer<std::uint8_t>(
            writer.bytes().at(static_cast<std::size_t>(index_offset))
        ),
        static_cast<std::uint8_t>(BlockType::Index)
    );
    EXPECT_EQ(
        read_u32_le(
            writer.bytes(),
            static_cast<std::size_t>(index_offset) + 1
        ),
        index.header.payload_size
    );
    EXPECT_EQ(
        read_u32_le(
            writer.bytes(),
            static_cast<std::size_t>(index_offset) + 5
        ),
        index.header.crc32
    );
    EXPECT_TRUE(index.validate().is_ok());
}

TEST(IndexSectionWriteTest, RejectsInvalidPayloadBeforePadding)
{
    const std::string key = "key";
    IndexSection index;
    ASSERT_TRUE(index.add_index(
        0,
        3,
        3,
        key.data(),
        key.data()
    ).is_ok());

    index.payloads[0].first_key_ptr = nullptr;

    MemoryWritableFile writer(100);
    std::uint64_t offset = 100;
    std::uint64_t index_offset = 999;

    const Status status = index.write(writer, offset, index_offset);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::NullPointer);
    EXPECT_EQ(writer.position(), 100u);
    EXPECT_EQ(writer.bytes().size(), 100u);
    EXPECT_EQ(index_offset, 999u);
}

TEST(IndexSectionWriteTest, CommitsDerivedStateOnlyAfterSuccess)
{
    const std::string key = "key";
    IndexSection index;
    ASSERT_TRUE(index.add_index(
        0,
        3,
        3,
        key.data(),
        key.data()
    ).is_ok());

    index.header.type = BlockType::Data;
    index.header.payload_size = 111;
    index.header.crc32 = 222;
    const IndexSection::Header stale_header = index.header;

    MemoryWritableFile writer(0, 5);
    std::uint64_t offset = 0;
    std::uint64_t index_offset = 999;

    const Status status = index.write(writer, offset, index_offset);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::WriteFailed);
    EXPECT_EQ(index_offset, 999u);
    EXPECT_EQ(index.header.type, stale_header.type);
    EXPECT_EQ(index.header.payload_size, stale_header.payload_size);
    EXPECT_EQ(index.header.crc32, stale_header.crc32);
}

TEST(IndexPayloadWriteTest, DoesNotSilentlyMoveToNextBlock)
{
    const std::string key = "k";
    IndexSection::Payload payload{};
    payload.data_block_offset = 0;
    payload.first_key_size = 1;
    payload.last_key_size = 1;
    payload.first_key_ptr = key.data();
    payload.last_key_ptr = key.data();

    MemoryWritableFile writer(BLOCK_SIZE - 10);
    std::uint64_t offset = BLOCK_SIZE - 10;

    const Status status = payload.write(writer, offset);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::SizeExceedsBlockBoundary);
    EXPECT_EQ(writer.position(), BLOCK_SIZE - 10);
}

TEST(IndexSectionLoadTest, StrictRoundTripUsesArenaOwnedKeys)
{
    IndexSection expected = make_three_range_index();
    constexpr std::uint64_t index_offset = 3ULL * BLOCK_SIZE;

    MemoryWritableFile writer(index_offset);
    std::uint64_t write_offset = index_offset;
    std::uint64_t committed_offset = 0;
    ASSERT_TRUE(expected.write(
        writer,
        write_offset,
        committed_offset
    ).is_ok());

    Arena arena;
    MemoryReadableFile reader(writer.bytes());
    std::uint64_t load_offset = 123;

    auto loaded = IndexSection::load(
        reader,
        load_offset,
        arena,
        index_offset,
        static_cast<std::uint32_t>(expected.disk_size()),
        0,
        3
    );

    ASSERT_TRUE(loaded.is_ok());
    EXPECT_EQ(
        load_offset,
        index_offset + expected.disk_size()
    );
    ASSERT_EQ(loaded.value.payloads.size(), 3u);
    EXPECT_EQ(
        bytes_view(
            loaded.value.payloads[0].first_key_ptr,
            loaded.value.payloads[0].first_key_size
        ),
        "a"
    );
    EXPECT_EQ(
        bytes_view(
            loaded.value.payloads[2].last_key_ptr,
            loaded.value.payloads[2].last_key_size
        ),
        "z"
    );
    EXPECT_TRUE(loaded.value.validate().is_ok());
}

TEST(IndexSectionLoadTest, EmptyIndexRoundTrips)
{
    IndexSection expected;
    MemoryWritableFile writer;
    std::uint64_t write_offset = 0;
    std::uint64_t index_offset = 999;

    ASSERT_TRUE(expected.write(
        writer,
        write_offset,
        index_offset
    ).is_ok());

    Arena arena;
    MemoryReadableFile reader(writer.bytes());
    std::uint64_t load_offset = 44;

    auto loaded = IndexSection::load(
        reader,
        load_offset,
        arena,
        0,
        static_cast<std::uint32_t>(expected.disk_size())
    );

    ASSERT_TRUE(loaded.is_ok());
    EXPECT_TRUE(loaded.value.payloads.empty());
    EXPECT_EQ(load_offset, IndexSection::Header::disk_size());
}

TEST(IndexSectionLoadTest, RejectsEveryTruncatedEncoding)
{
    const auto complete = encode_index({
        RawEntry{0, "alpha", "omega"}
        });

    for (std::size_t length = 0; length < complete.size(); ++length) {
        std::vector<std::byte> truncated(
            complete.begin(),
            complete.begin() + static_cast<std::ptrdiff_t>(length)
        );

        Arena arena;
        MemoryReadableFile reader(std::move(truncated));
        std::uint64_t offset = 77;

        const auto loaded = IndexSection::load(
            reader,
            offset,
            arena,
            0,
            static_cast<std::uint32_t>(complete.size())
        );

        EXPECT_FALSE(loaded.is_ok());
        EXPECT_EQ(offset, 77u);
    }
}

TEST(IndexSectionLoadTest, ChecksumFailureRestoresOffsetAndArena)
{
    auto bytes = encode_index({
        RawEntry{0, "alpha", "omega"}
        });
    bytes[IndexSection::Header::disk_size() + 1] ^= std::byte{ 0x01 };

    Arena arena;
    const std::uint64_t used_before = arena.get_used_bytes().value;
    MemoryReadableFile reader(std::move(bytes));
    std::uint64_t offset = 88;

    const auto loaded = IndexSection::load(
        reader,
        offset,
        arena,
        0
    );

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::ChecksumMismatch);
    EXPECT_EQ(offset, 88u);
    EXPECT_EQ(arena.get_used_bytes().value, used_before);
}

TEST(IndexSectionLoadTest, ChecksCrcBeforeInterpretingPayload)
{
    auto bytes = encode_index({
        RawEntry{0, "a", "z"}
        });

    // Makes data_block_offset unaligned, but deliberately leaves the old CRC.
    bytes[IndexSection::Header::disk_size()] ^= std::byte{ 0x01 };

    Arena arena;
    MemoryReadableFile reader(std::move(bytes));
    std::uint64_t offset = 12;

    const auto loaded = IndexSection::load(
        reader,
        offset,
        arena,
        0
    );

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::ChecksumMismatch);
    EXPECT_EQ(offset, 12u);
}

TEST(IndexSectionLoadTest, RejectsUnalignedDataOffsetWithValidCrc)
{
    auto bytes = encode_index({
        RawEntry{1, "a", "z"}
        });

    Arena arena;
    MemoryReadableFile reader(std::move(bytes));
    std::uint64_t offset = 9;

    const auto loaded = IndexSection::load(
        reader,
        offset,
        arena,
        0
    );

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::InvalidSectionOffset);
    EXPECT_EQ(offset, 9u);
}

TEST(IndexSectionLoadTest, RejectsReversedRangeWithValidCrc)
{
    auto bytes = encode_index({
        RawEntry{0, "z", "a"}
        });

    Arena arena;
    MemoryReadableFile reader(std::move(bytes));
    std::uint64_t offset = 9;

    const auto loaded = IndexSection::load(
        reader,
        offset,
        arena,
        0
    );

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::Corruption);
    EXPECT_EQ(offset, 9u);
}

TEST(IndexSectionLoadTest, RejectsNonconsecutiveOffsetsWithValidCrc)
{
    auto bytes = encode_index({
        RawEntry{0, "a", "b"},
        RawEntry{2ULL * BLOCK_SIZE, "c", "d"}
        });

    Arena arena;
    MemoryReadableFile reader(std::move(bytes));
    std::uint64_t offset = 9;

    const auto loaded = IndexSection::load(
        reader,
        offset,
        arena,
        0
    );

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::InvalidSectionOffset);
    EXPECT_EQ(offset, 9u);
}

TEST(IndexSectionLoadTest, RejectsOutOfOrderRangesWithValidCrc)
{
    auto bytes = encode_index({
        RawEntry{0, "m", "z"},
        RawEntry{BLOCK_SIZE, "a", "b"}
        });

    Arena arena;
    MemoryReadableFile reader(std::move(bytes));
    std::uint64_t offset = 9;

    const auto loaded = IndexSection::load(
        reader,
        offset,
        arena,
        0
    );

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::OverlappingKeys);
    EXPECT_EQ(offset, 9u);
}

TEST(IndexSectionLoadTest, RejectsFooterSizeMismatch)
{
    auto bytes = encode_index({
        RawEntry{0, "a", "z"}
        });

    Arena arena;
    MemoryReadableFile reader(std::move(bytes));
    std::uint64_t offset = 7;

    const auto loaded = IndexSection::load(
        reader,
        offset,
        arena,
        0,
        999
    );

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::InvalidSectionSize);
    EXPECT_EQ(offset, 7u);
}

TEST(IndexSectionLoadTest, StrictLoadRejectsWrongDataBlockCount)
{
    constexpr std::uint64_t index_offset = 2ULL * BLOCK_SIZE;
    auto bytes = encode_index({
        RawEntry{0, "a", "b"},
        RawEntry{BLOCK_SIZE, "c", "d"}
        }, index_offset);

    Arena arena;
    MemoryReadableFile reader(std::move(bytes));
    std::uint64_t offset = 7;

    const auto loaded = IndexSection::load(
        reader,
        offset,
        arena,
        index_offset,
        0,
        0,
        1
    );

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::InvalidSectionOffset);
    EXPECT_EQ(offset, 7u);
}

TEST(IndexSectionLoadTest, StrictLoadRejectsEntryCountMismatch)
{
    constexpr std::uint64_t index_offset = 2ULL * BLOCK_SIZE;
    auto bytes = encode_index({
        RawEntry{0, "a", "b"}
        }, index_offset);

    Arena arena;
    MemoryReadableFile reader(std::move(bytes));
    std::uint64_t offset = 7;

    const auto loaded = IndexSection::load(
        reader,
        offset,
        arena,
        index_offset,
        0,
        0,
        2
    );

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::InvalidSectionSize);
    EXPECT_EQ(offset, 7u);
}