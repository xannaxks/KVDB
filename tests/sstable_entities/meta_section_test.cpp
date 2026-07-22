#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "arena.h"
#include "crc32_helpers.h"
#include "record.h"
#include "sstable_entities.h"
#include "sstable_entities/data_section.h"
#include "sstable_entities/index_section.h"
#include "sstable_entities/meta_section.h"

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

            if (bytes_written_ + size > fail_after_bytes_) {
                return Status{
                    StatusCode::WriteFailed,
                    "injected write failure"
                };
            }

            if (size > 0 && data == nullptr) {
                return Status{
                    StatusCode::NullPointer,
                    "test writable received null data"
                };
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

            if (offset >= bytes_.size()) {
                return Status::ok();
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

    struct BuiltData
    {
        StoredRecord alpha{ "alpha", "one", Type::Put, 9 };
        StoredRecord beta{ "beta", {}, Type::Tombstone, 4 };
        StoredRecord omega{ "omega", "three", Type::Put, 2 };
        DataSection data;
        IndexSection index;

        BuiltData()
        {
            EXPECT_TRUE(data.add_payload(alpha.record).is_ok());
            EXPECT_TRUE(data.add_payload(beta.record).is_ok());
            EXPECT_TRUE(data.add_payload(omega.record).is_ok());

            std::uint64_t block_offset = 0;
            for (const DataSection::DataBlock& block : data.data_blocks) {
                const DataSection::Payload& first = block.payloads.front();
                const DataSection::Payload& last = block.payloads.back();
                index.add_index(
                    block_offset,
                    first.key_size,
                    last.key_size,
                    first.key_ptr,
                    last.key_ptr
                );
                block_offset += BLOCK_SIZE;
            }
        }
    };

    [[nodiscard]] std::string_view bytes_view(
        const void* data,
        std::uint32_t size
    )
    {
        return std::string_view(
            static_cast<const char*>(data),
            size
        );
    }

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

    void append_u8(std::vector<std::byte>& out, std::uint8_t value)
    {
        out.push_back(static_cast<std::byte>(value));
    }

    void append_u32(std::vector<std::byte>& out, std::uint32_t value)
    {
        for (unsigned shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
        }
    }

    void append_u64(std::vector<std::byte>& out, std::uint64_t value)
    {
        for (unsigned shift = 0; shift < 64; shift += 8) {
            out.push_back(static_cast<std::byte>((value >> shift) & 0xFFull));
        }
    }

    struct RawMeta
    {
        std::uint64_t record_count = 1;
        std::uint64_t tombstone_count = 0;
        std::uint64_t min_seq = 1;
        std::uint64_t max_seq = 1;
        std::string min_key = "a";
        std::string max_key = "z";
        std::uint64_t block_count = 1;
        std::uint64_t data_bytes =
            DataSection::Header::disk_size() +
            DataSection::Payload::fixed_part_disk_size() + 2;
    };

    [[nodiscard]] std::vector<std::byte> encode_raw_meta(
        const RawMeta& raw,
        BlockType type = BlockType::Meta
    )
    {
        std::vector<std::byte> payload;
        append_u64(payload, raw.record_count);
        append_u64(payload, raw.tombstone_count);
        append_u64(payload, raw.min_seq);
        append_u64(payload, raw.max_seq);
        append_u32(payload, static_cast<std::uint32_t>(raw.min_key.size()));
        append_u32(payload, static_cast<std::uint32_t>(raw.max_key.size()));
        append_u64(payload, raw.block_count);
        append_u64(payload, raw.data_bytes);
        payload.insert(
            payload.end(),
            reinterpret_cast<const std::byte*>(raw.min_key.data()),
            reinterpret_cast<const std::byte*>(raw.min_key.data()) +
            raw.min_key.size()
        );
        payload.insert(
            payload.end(),
            reinterpret_cast<const std::byte*>(raw.max_key.data()),
            reinterpret_cast<const std::byte*>(raw.max_key.data()) +
            raw.max_key.size()
        );

        std::vector<std::byte> encoded;
        append_u8(encoded, static_cast<std::uint8_t>(type));
        append_u32(encoded, static_cast<std::uint32_t>(payload.size()));
        append_u32(encoded, crc32_of(payload.data(), payload.size()));
        encoded.insert(encoded.end(), payload.begin(), payload.end());
        return encoded;
    }

    [[nodiscard]] IndexSection make_index(
        std::string& first,
        std::string& last,
        std::size_t count = 1
    )
    {
        IndexSection index;
        for (std::size_t i = 0; i < count; ++i) {
            index.add_index(
                static_cast<std::uint64_t>(i) * BLOCK_SIZE,
                static_cast<std::uint32_t>(first.size()),
                static_cast<std::uint32_t>(last.size()),
                first.empty() ? nullptr : first.data(),
                last.empty() ? nullptr : last.data()
            );
        }
        return index;
    }

    [[nodiscard]] std::uint64_t used_bytes(Arena& arena)
    {
        Result<std::uint64_t> used = arena.get_used_bytes();
        EXPECT_TRUE(used.is_ok());
        return used.is_ok() ? used.value : 0;
    }
}

TEST(MetaSectionLayoutTest, UsesStableSerializedWidths)
{
    EXPECT_EQ(MetaSection::Header::disk_size(), 9u);
    EXPECT_EQ(MetaSection::Payload::fixed_disk_size(), 56u);
    EXPECT_EQ(MetaSection::fixed_disk_size(), 65u);
}

TEST(MetaSectionTest, DefaultConstructionIsCanonicalEmptyMetadata)
{
    MetaSection meta;
    EXPECT_EQ(meta.header.type, BlockType::Meta);
    EXPECT_EQ(meta.header.payload_size, MetaSection::Payload::fixed_disk_size());
    EXPECT_EQ(meta.payload.record_count, 0u);
    EXPECT_EQ(meta.payload.data_block_count, 0u);
    EXPECT_TRUE(meta.payload.validate().is_ok());

    std::uint32_t crc = 0;
    meta.payload.calculate_crc32(crc);
    EXPECT_EQ(meta.header.crc32, crc);
}

TEST(MetaSectionTest, RebuildAggregatesCountsRangesSequencesAndBytes)
{
    BuiltData built;
    MetaSection meta;
    const Status status = meta.rebuild(built.data, built.index);

    ASSERT_TRUE(status.is_ok());
    EXPECT_EQ(meta.payload.record_count, 3u);
    EXPECT_EQ(meta.payload.tombstone_count, 1u);
    EXPECT_EQ(meta.payload.min_seq_num, 2u);
    EXPECT_EQ(meta.payload.max_seq_num, 9u);
    EXPECT_EQ(bytes_view(meta.payload.min_key_ptr, meta.payload.min_key_size), "alpha");
    EXPECT_EQ(bytes_view(meta.payload.max_key_ptr, meta.payload.max_key_size), "omega");
    EXPECT_EQ(meta.payload.data_block_count, built.data.data_blocks.size());
    EXPECT_EQ(meta.payload.data_bytes, built.data.disk_size());
    EXPECT_TRUE(meta.validate(built.index).is_ok());
}

TEST(MetaSectionTest, FailedRebuildPreservesExistingMetadata)
{
    BuiltData built;
    MetaSection meta;
    ASSERT_TRUE(meta.rebuild(built.data, built.index).is_ok());
    const MetaSection before = meta;

    IndexSection wrong_index;
    const Status status = meta.rebuild(built.data, wrong_index);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::InvariantViolation);
    EXPECT_EQ(meta.payload.record_count, before.payload.record_count);
    EXPECT_EQ(meta.header.crc32, before.header.crc32);
    EXPECT_EQ(meta.payload.min_key_ptr, before.payload.min_key_ptr);
}

TEST(MetaSectionTest, RebuildRejectsEmptyPhysicalDataBlock)
{
    DataSection data;
    data.init_new_block();
    IndexSection index;
    std::string empty;
    index.add_index(0, 0, 0, nullptr, nullptr);

    MetaSection meta;
    const Status status = meta.rebuild(data, index);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::InvalidState);
}

TEST(MetaSectionTest, RebuildRejectsMalformedRecordBeforeMutation)
{
    BuiltData built;
    built.data.data_blocks.front().payloads.front().key_ptr = nullptr;

    MetaSection meta;
    const Status status = meta.rebuild(built.data, built.index);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::NullPointer);
    EXPECT_EQ(meta.payload.record_count, 0u);
}

TEST(MetaSectionTest, RejectsBoundaryKeysThatCannotFitInOneMetaBlock)
{
    StoredRecord first(std::string(2100, 'a'), {}, Type::Put, 2);
    StoredRecord last(std::string(2100, 'z'), {}, Type::Put, 1);
    DataSection data;
    ASSERT_TRUE(data.add_payload(first.record).is_ok());
    ASSERT_TRUE(data.add_payload(last.record).is_ok());
    ASSERT_EQ(data.data_blocks.size(), 2u);

    IndexSection index;
    index.add_index(0, first.record.key_entry.size, first.record.key_entry.size,
        first.record.key_entry.data, first.record.key_entry.data);
    index.add_index(BLOCK_SIZE, last.record.key_entry.size, last.record.key_entry.size,
        last.record.key_entry.data, last.record.key_entry.data);

    MetaSection meta;
    const Status status = meta.rebuild(data, index);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::InvalidPayloadSize);
}

TEST(MetaSectionTest, WriteAlignsAndRebuildsStaleHeader)
{
    BuiltData built;
    MetaSection meta;
    ASSERT_TRUE(meta.rebuild(built.data, built.index).is_ok());
    meta.header.type = BlockType::Index;
    meta.header.payload_size = 1;
    meta.header.crc32 = 2;

    MemoryWritableFile file(24);
    std::uint64_t offset = 24;
    std::uint64_t meta_offset = 999;
    const Status status = meta.write(file, offset, meta_offset);

    ASSERT_TRUE(status.is_ok());
    EXPECT_EQ(meta_offset, BLOCK_SIZE);
    EXPECT_EQ(meta.header.type, BlockType::Meta);
    EXPECT_EQ(meta.header.payload_size, meta.payload.disk_size());
    EXPECT_EQ(
        std::to_integer<std::uint8_t>(file.bytes().at(BLOCK_SIZE)),
        static_cast<std::uint8_t>(BlockType::Meta)
    );
    EXPECT_EQ(read_u32_le(file.bytes(), BLOCK_SIZE + 1), meta.payload.disk_size());
    EXPECT_EQ(read_u32_le(file.bytes(), BLOCK_SIZE + 5), meta.header.crc32);
}

TEST(MetaSectionTest, InvalidPayloadIsRejectedBeforeAnyWrite)
{
    MetaSection meta;
    meta.payload.record_count = 1;
    meta.payload.data_block_count = 1;
    meta.payload.data_bytes = 100;
    meta.payload.min_key_size = 1;
    meta.payload.max_key_size = 1;
    meta.payload.min_key_ptr = nullptr;
    std::string max = "z";
    meta.payload.max_key_ptr = max.data();

    MemoryWritableFile file;
    std::uint64_t offset = 0;
    std::uint64_t meta_offset = 55;
    const Status status = meta.write(file, offset, meta_offset);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::NullPointer);
    EXPECT_EQ(offset, 0u);
    EXPECT_EQ(meta_offset, 55u);
    EXPECT_TRUE(file.bytes().empty());
}

TEST(MetaSectionPayloadTest, DoesNotSilentlyAlignAcrossBlockBoundary)
{
    BuiltData built;
    MetaSection meta;
    ASSERT_TRUE(meta.rebuild(built.data, built.index).is_ok());

    constexpr std::uint64_t start = BLOCK_SIZE - 20;
    MemoryWritableFile file(start);
    std::uint64_t offset = start;
    const Status status = meta.payload.write(file, offset);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::SizeExceedsBlockBoundary);
    EXPECT_EQ(offset, start);
}

TEST(MetaSectionTest, FailedWritePreservesMetaOffsetAndCachedHeader)
{
    BuiltData built;
    MetaSection meta;
    ASSERT_TRUE(meta.rebuild(built.data, built.index).is_ok());
    meta.header.crc32 = 123;

    MemoryWritableFile file(0, 20);
    std::uint64_t offset = 0;
    std::uint64_t meta_offset = 777;
    const Status status = meta.write(file, offset, meta_offset);

    ASSERT_FALSE(status.is_ok());
    EXPECT_EQ(status.code, StatusCode::WriteFailed);
    EXPECT_EQ(meta_offset, 777u);
    EXPECT_EQ(meta.header.crc32, 123u);
    EXPECT_GT(offset, 0u);
}

TEST(MetaSectionTest, RoundTripsAndAllocatesBoundaryKeysInArena)
{
    BuiltData built;
    MetaSection expected;
    ASSERT_TRUE(expected.rebuild(built.data, built.index).is_ok());

    MemoryWritableFile writer;
    std::uint64_t write_offset = 0;
    std::uint64_t meta_offset = 0;
    ASSERT_TRUE(expected.write(writer, write_offset, meta_offset).is_ok());

    MemoryReadableFile reader(writer.bytes());
    Arena arena;
    std::uint64_t read_offset = 999;
    Result<MetaSection> loaded = MetaSection::load(
        reader,
        read_offset,
        built.index,
        arena,
        meta_offset
    );

    ASSERT_TRUE(loaded.is_ok());
    EXPECT_EQ(read_offset, expected.disk_size());
    EXPECT_EQ(loaded.value.payload.record_count, 3u);
    EXPECT_EQ(bytes_view(loaded.value.payload.min_key_ptr,
        loaded.value.payload.min_key_size), "alpha");
    EXPECT_EQ(bytes_view(loaded.value.payload.max_key_ptr,
        loaded.value.payload.max_key_size), "omega");
    EXPECT_EQ(used_bytes(arena), 10u);
}

TEST(MetaSectionTest, ChecksumFailureRollsBackOffsetAndArena)
{
    BuiltData built;
    MetaSection meta;
    ASSERT_TRUE(meta.rebuild(built.data, built.index).is_ok());

    MemoryWritableFile writer;
    std::uint64_t write_offset = 0;
    std::uint64_t meta_offset = 0;
    ASSERT_TRUE(meta.write(writer, write_offset, meta_offset).is_ok());

    std::vector<std::byte> corrupted = writer.bytes();
    corrupted.back() ^= std::byte{ 1 };

    MemoryReadableFile reader(std::move(corrupted));
    Arena arena;
    ASSERT_TRUE(arena.alloc(13, alignof(std::byte)).is_ok());
    const std::uint64_t used_before = used_bytes(arena);
    std::uint64_t offset = 321;

    Result<MetaSection> loaded = MetaSection::load(
        reader,
        offset,
        built.index,
        arena,
        meta_offset
    );

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::ChecksumMismatch);
    EXPECT_EQ(offset, 321u);
    EXPECT_EQ(used_bytes(arena), used_before);
}

TEST(MetaSectionTest, EveryTruncatedEncodingFailsTransactionally)
{
    BuiltData built;
    MetaSection meta;
    ASSERT_TRUE(meta.rebuild(built.data, built.index).is_ok());

    MemoryWritableFile writer;
    std::uint64_t write_offset = 0;
    std::uint64_t meta_offset = 0;
    ASSERT_TRUE(meta.write(writer, write_offset, meta_offset).is_ok());

    for (std::size_t size = 0; size < writer.bytes().size(); ++size) {
        std::vector<std::byte> truncated(
            writer.bytes().begin(),
            writer.bytes().begin() + static_cast<std::ptrdiff_t>(size)
        );
        MemoryReadableFile reader(std::move(truncated));
        Arena arena;
        const std::uint64_t used_before = used_bytes(arena);
        std::uint64_t offset = 77;

        Result<MetaSection> loaded = MetaSection::load(
            reader,
            offset,
            built.index,
            arena,
            meta_offset
        );

        EXPECT_FALSE(loaded.is_ok());
        EXPECT_EQ(offset, 77u);
        EXPECT_EQ(used_bytes(arena), used_before);
    }
}

TEST(MetaSectionTest, RejectsWrongBlockType)
{
    RawMeta raw;
    std::vector<std::byte> bytes = encode_raw_meta(raw, BlockType::Index);
    std::string first = "a";
    std::string last = "z";
    IndexSection index = make_index(first, last);
    MemoryReadableFile reader(std::move(bytes));
    Arena arena;
    std::uint64_t offset = 44;

    Result<MetaSection> loaded = MetaSection::load(reader, offset, index, arena, 0);

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::InvalidBlockType);
    EXPECT_EQ(offset, 44u);
}

TEST(MetaSectionTest, RejectsHeaderPayloadSizeBelowFixedFields)
{
    RawMeta raw;
    std::vector<std::byte> bytes = encode_raw_meta(raw);
    bytes[1] = std::byte{ 1 };
    bytes[2] = std::byte{ 0 };
    bytes[3] = std::byte{ 0 };
    bytes[4] = std::byte{ 0 };

    std::string first = "a";
    std::string last = "z";
    IndexSection index = make_index(first, last);
    MemoryReadableFile reader(std::move(bytes));
    Arena arena;
    std::uint64_t offset = 9;

    Result<MetaSection> loaded = MetaSection::load(reader, offset, index, arena, 0);

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::InvalidPayloadSize);
    EXPECT_EQ(offset, 9u);
}

TEST(MetaSectionTest, RejectsTombstoneCountAboveRecordCountWithValidCRC)
{
    RawMeta raw;
    raw.tombstone_count = 2;
    std::vector<std::byte> bytes = encode_raw_meta(raw);
    std::string first = "a";
    std::string last = "z";
    IndexSection index = make_index(first, last);
    MemoryReadableFile reader(std::move(bytes));
    Arena arena;
    std::uint64_t offset = 5;

    Result<MetaSection> loaded = MetaSection::load(reader, offset, index, arena, 0);

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::InvalidPayloadSize);
    EXPECT_EQ(offset, 5u);
    EXPECT_EQ(used_bytes(arena), 0u);
}

TEST(MetaSectionTest, RejectsInvertedSequenceRangeWithValidCRC)
{
    RawMeta raw;
    raw.min_seq = 9;
    raw.max_seq = 2;
    std::vector<std::byte> bytes = encode_raw_meta(raw);
    std::string first = "a";
    std::string last = "z";
    IndexSection index = make_index(first, last);
    MemoryReadableFile reader(std::move(bytes));
    Arena arena;
    std::uint64_t offset = 6;

    Result<MetaSection> loaded = MetaSection::load(reader, offset, index, arena, 0);

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::InvalidPayloadSize);
    EXPECT_EQ(offset, 6u);
}

TEST(MetaSectionTest, RejectsInvertedKeyRangeWithValidCRC)
{
    RawMeta raw;
    raw.min_key = "z";
    raw.max_key = "a";
    std::vector<std::byte> bytes = encode_raw_meta(raw);
    std::string first = "a";
    std::string last = "z";
    IndexSection index = make_index(first, last);
    MemoryReadableFile reader(std::move(bytes));
    Arena arena;
    std::uint64_t offset = 7;

    Result<MetaSection> loaded = MetaSection::load(reader, offset, index, arena, 0);

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::InvalidPayloadSize);
    EXPECT_EQ(offset, 7u);
}

TEST(MetaSectionTest, RejectsNonCanonicalEmptyMetadataWithValidCRC)
{
    RawMeta raw;
    raw.record_count = 0;
    raw.tombstone_count = 0;
    raw.min_seq = 0;
    raw.max_seq = 0;
    raw.min_key.clear();
    raw.max_key.clear();
    raw.block_count = 0;
    raw.data_bytes = 1;
    std::vector<std::byte> bytes = encode_raw_meta(raw);
    IndexSection index;
    MemoryReadableFile reader(std::move(bytes));
    Arena arena;
    std::uint64_t offset = 8;

    Result<MetaSection> loaded = MetaSection::load(reader, offset, index, arena, 0);

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::InvalidPayloadSize);
    EXPECT_EQ(offset, 8u);
}

TEST(MetaSectionTest, RejectsImpossibleDataByteCountWithValidCRC)
{
    RawMeta raw;
    raw.data_bytes = 1;
    std::vector<std::byte> bytes = encode_raw_meta(raw);
    std::string first = "a";
    std::string last = "z";
    IndexSection index = make_index(first, last);
    MemoryReadableFile reader(std::move(bytes));
    Arena arena;
    std::uint64_t offset = 10;

    Result<MetaSection> loaded = MetaSection::load(reader, offset, index, arena, 0);

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::InvalidPayloadSize);
    EXPECT_EQ(offset, 10u);
}

TEST(MetaSectionTest, RejectsIndexCountMismatchAndRollsBackArena)
{
    RawMeta raw;
    std::vector<std::byte> bytes = encode_raw_meta(raw);
    IndexSection empty_index;
    MemoryReadableFile reader(std::move(bytes));
    Arena arena;
    std::uint64_t offset = 11;

    Result<MetaSection> loaded = MetaSection::load(
        reader,
        offset,
        empty_index,
        arena,
        0
    );

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::InvariantViolation);
    EXPECT_EQ(offset, 11u);
    EXPECT_EQ(used_bytes(arena), 0u);
}

TEST(MetaSectionTest, RejectsIndexBoundaryMismatch)
{
    RawMeta raw;
    std::vector<std::byte> bytes = encode_raw_meta(raw);
    std::string first = "b";
    std::string last = "z";
    IndexSection index = make_index(first, last);
    MemoryReadableFile reader(std::move(bytes));
    Arena arena;
    std::uint64_t offset = 12;

    Result<MetaSection> loaded = MetaSection::load(reader, offset, index, arena, 0);

    ASSERT_FALSE(loaded.is_ok());
    EXPECT_EQ(loaded.status.code, StatusCode::InvariantViolation);
    EXPECT_EQ(offset, 12u);
    EXPECT_EQ(used_bytes(arena), 0u);
}

TEST(MetaSectionTest, CanonicalEmptyMetadataRoundTrips)
{
    MetaSection meta;
    IndexSection index;
    MemoryWritableFile writer;
    std::uint64_t write_offset = 0;
    std::uint64_t meta_offset = 99;
    ASSERT_TRUE(meta.write(writer, write_offset, meta_offset).is_ok());

    MemoryReadableFile reader(writer.bytes());
    Arena arena;
    std::uint64_t read_offset = 55;
    Result<MetaSection> loaded = MetaSection::load(
        reader,
        read_offset,
        index,
        arena,
        meta_offset
    );

    ASSERT_TRUE(loaded.is_ok());
    EXPECT_EQ(loaded.value.payload.record_count, 0u);
    EXPECT_EQ(read_offset, MetaSection::fixed_disk_size());
    EXPECT_EQ(used_bytes(arena), 0u);
}