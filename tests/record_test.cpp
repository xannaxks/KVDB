#include "arena.h"
#include "record.h"
#include <gtest/gtest.h>


TEST(InternalRecordIOTest, RoundTripsPutRecord)
{
    Arena arena;

    auto key = ArenaEntry::make_entry(arena, "key");
    auto value = ArenaEntry::make_entry(arena, "value");

    InternalRecord original(key, value, Type::Put, 123);

    std::ofstream out("record_test.bin", std::ios::binary);
    ASSERT_TRUE(original.write(out));
    out.close();

    Arena read_arena;
    std::ifstream in("record_test.bin", std::ios::binary);

    auto loaded = InternalRecord::read(in, read_arena);

    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(*loaded, original);
}

TEST(InternalRecordIOTest, RoundTripsTombstoneRecord)
{
    Arena arena;

    auto key = ArenaEntry::make_entry(arena, "deleted_key");
    auto value = ArenaEntry::make_entry(arena, "");

    InternalRecord original(key, value, Type::Tombstone, 999);

    std::ofstream out("tombstone_test.bin", std::ios::binary);
    ASSERT_TRUE(original.write(out));
    out.close();

    Arena read_arena;
    std::ifstream in("tombstone_test.bin", std::ios::binary);

    auto loaded = InternalRecord::read(in, read_arena);

    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(*loaded, original);
}

TEST(InternalRecordIOTest, RoundTripsEmptyKeyAndValue)
{
    Arena arena;

    auto key = ArenaEntry::make_entry(arena, "");
    auto value = ArenaEntry::make_entry(arena, "");

    InternalRecord original(key, value, Type::Put, 1);

    std::ofstream out("empty_record_test.bin", std::ios::binary);
    ASSERT_TRUE(original.write(out));
    out.close();

    Arena read_arena;
    std::ifstream in("empty_record_test.bin", std::ios::binary);

    auto loaded = InternalRecord::read(in, read_arena);

    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(*loaded, original);
}

TEST(InternalRecordIOTest, RoundTripsBinaryKeyAndValue)
{
    Arena arena;

    std::string key{ "a\0b\1c", 5 };
    std::string value{ "x\0y\2z", 5 };

    auto key_entry = ArenaEntry::make_entry(arena, key);
    auto value_entry = ArenaEntry::make_entry(arena, value);

    InternalRecord original(key_entry, value_entry, Type::Put, 42);

    std::ofstream out("binary_record_test.bin", std::ios::binary);
    ASSERT_TRUE(original.write(out));
    out.close();

    Arena read_arena;
    std::ifstream in("binary_record_test.bin", std::ios::binary);

    auto loaded = InternalRecord::read(in, read_arena);

    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(*loaded, original);
}

TEST(InternalRecordIOTest, ReadRejectsInvalidType)
{
    std::ofstream out("invalid_type_test.bin", std::ios::binary);

    ASSERT_TRUE(kvdb::endian::write_u64_le(out, 123));
    ASSERT_TRUE(kvdb::endian::write_u8(out, 99)); // invalid type
    out.close();

    Arena arena;
    std::ifstream in("invalid_type_test.bin", std::ios::binary);

    auto loaded = InternalRecord::read(in, arena);

    EXPECT_FALSE(loaded.has_value());
}

TEST(InternalRecordIOTest, ReadReturnsNulloptWhenValueIsMissing)
{
    std::ofstream out("missing_value_test.bin", std::ios::binary);

    ASSERT_TRUE(kvdb::endian::write_u64_le(out, 123));
    ASSERT_TRUE(kvdb::endian::write_u8(out, static_cast<std::uint8_t>(Type::Put)));
    ASSERT_TRUE(kvdb::endian::write_bytes_with_u32_size(
        out,
        std::span<const std::byte>{}
    ));

    out.close();

    Arena arena;
    std::ifstream in("missing_value_test.bin", std::ios::binary);

    auto loaded = InternalRecord::read(in, arena);

    EXPECT_FALSE(loaded.has_value());
}