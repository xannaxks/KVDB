#include <gtest/gtest.h>
#include "endian_io.h"
#include <string_view>
#include <array>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <span>
#include <string>
#include <arena.h>
#include <vector>
#include <record.h>

TEST(EndianIOTest, PutU8WritesExpectedByte)
{
    std::vector<std::byte> out;

    kvdb::endian::put_u8(out, 0x12u);

    ASSERT_EQ(out.size(), 1);
    EXPECT_EQ(out[0], std::byte{ 0x12 });
}

TEST(EndianIOTest, PutU16LEWritesExpectedBytes)
{
    std::vector<std::byte> out;

    kvdb::endian::put_u16_le(out, 0x1234u);

    ASSERT_EQ(out.size(), 2);

    EXPECT_EQ(out[0], std::byte{ 0x34 });
    EXPECT_EQ(out[1], std::byte{ 0x12 });
}

TEST(EndianIOTest, PutU32LEWritesExpectedBytes)
{
    std::vector<std::byte> out;

    kvdb::endian::put_u32_le(out, 0x12345678u);

    ASSERT_EQ(out.size(), 4);

    EXPECT_EQ(out[0], std::byte{ 0x78 });
    EXPECT_EQ(out[1], std::byte{ 0x56 });
    EXPECT_EQ(out[2], std::byte{ 0x34 });
    EXPECT_EQ(out[3], std::byte{ 0x12 });
}

TEST(EndianIOTest, PutU64LEWritesExpectedBytes)
{
    std::vector<std::byte> out;

    kvdb::endian::put_u64_le(out, 0x1122334455667788ull);

    ASSERT_EQ(out.size(), 8);

    EXPECT_EQ(out[0], std::byte{ 0x88 });
    EXPECT_EQ(out[1], std::byte{ 0x77 });
    EXPECT_EQ(out[2], std::byte{ 0x66 });
    EXPECT_EQ(out[3], std::byte{ 0x55 });
    EXPECT_EQ(out[4], std::byte{ 0x44 });
    EXPECT_EQ(out[5], std::byte{ 0x33 });
    EXPECT_EQ(out[6], std::byte{ 0x22 });
    EXPECT_EQ(out[7], std::byte{ 0x11 });
}

TEST(EndianIOTest, ReaderRoundTripsIntegersAndSizedBytes)
{
    std::vector<std::byte> out;

    kvdb::endian::put_u8(out, 0xAB);
    kvdb::endian::put_u16_le(out, 0x1234);
    kvdb::endian::put_u32_le(out, 0x12345678);
    kvdb::endian::put_u64_le(out, 0x1122334455667788ull);

    std::vector<std::byte> payload = {
        std::byte{0xFF},
        std::byte{0x00},
        std::byte{0xCD},
        std::byte{0xFF}
    };

    kvdb::endian::put_bytes_with_u32_size(out, payload);

    kvdb::endian::Reader reader(out);

    auto a = reader.read_u8();
    auto b = reader.read_u16_le();
    auto c = reader.read_u32_le();
    auto d = reader.read_u64_le();
    auto e = reader.read_bytes_with_u32_size();

    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_TRUE(c.has_value());
    ASSERT_TRUE(d.has_value());
    ASSERT_TRUE(e.has_value());

    EXPECT_EQ(*a, 0xAB);
    EXPECT_EQ(*b, 0x1234);
    EXPECT_EQ(*c, 0x12345678);
    EXPECT_EQ(*d, 0x1122334455667788ull);

    // read_bytes_with_u32_size() returns only the payload, not the 4-byte size prefix.
    EXPECT_EQ(*e, payload);

    EXPECT_TRUE(reader.finished());
}

TEST(EndianIOTest, PutBytesWithU32SizeWritesSizeThenBytes)
{
    std::vector<std::byte> out;

    std::array<std::byte, 3> bytes{
        std::byte{'c'},
        std::byte{'a'},
        std::byte{'t'}
    };

    kvdb::endian::put_bytes_with_u32_size(out, bytes);

    ASSERT_EQ(out.size(), 7);

    // size = 3 as u32 little-endian
    EXPECT_EQ(out[0], std::byte{ 0x03 });
    EXPECT_EQ(out[1], std::byte{ 0x00 });
    EXPECT_EQ(out[2], std::byte{ 0x00 });
    EXPECT_EQ(out[3], std::byte{ 0x00 });

    // bytes = "cat"
    EXPECT_EQ(out[4], std::byte{ 'c' });
    EXPECT_EQ(out[5], std::byte{ 'a' });
    EXPECT_EQ(out[6], std::byte{ 't' });
}

TEST(EndianIOTest, ReaderReturnsNulloptWhenU32IsTruncated)
{
    std::vector<std::byte> data{
        std::byte{0x78},
        std::byte{0x56},
        std::byte{0x34}
    };

    kvdb::endian::Reader reader(data);

    auto value = reader.read_u32_le();

    EXPECT_FALSE(value.has_value());
    EXPECT_EQ(reader.position(), 0);
}

TEST(EndianIOTest, ReaderReturnsNulloptWhenSizedBytesAreTruncated)
{
    std::vector<std::byte> data;

    // Claims size = 5
    kvdb::endian::put_u32_le(data, 5);

    // But only provides 2 bytes
    data.push_back(std::byte{ 'h' });
    data.push_back(std::byte{ 'i' });

    kvdb::endian::Reader reader(data);

    auto bytes = reader.read_bytes_with_u32_size();

    EXPECT_FALSE(bytes.has_value());
}

TEST(EndianIOTest, ReaderHandlesEmptySizedBytes)
{
    std::vector<std::byte> out;

    std::span<const std::byte> empty;
    kvdb::endian::put_bytes_with_u32_size(out, empty);

    // Encoded form is 4 bytes: 00 00 00 00
    ASSERT_EQ(out.size(), 4);
    EXPECT_EQ(out[0], std::byte{ 0x00 });
    EXPECT_EQ(out[1], std::byte{ 0x00 });
    EXPECT_EQ(out[2], std::byte{ 0x00 });
    EXPECT_EQ(out[3], std::byte{ 0x00 });

    kvdb::endian::Reader reader(out);

    auto decoded = reader.read_bytes_with_u32_size();

    ASSERT_TRUE(decoded.has_value());

    // Decoded payload is empty.
    EXPECT_TRUE(decoded->empty());
    EXPECT_EQ(decoded->size(), 0);

    EXPECT_TRUE(reader.finished());
}

TEST(EndianIOTest, FileRoundTripsU32)
{
    const std::string path = "tmp_endian_test.bin";

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());

        ASSERT_TRUE(kvdb::endian::write_u32_le(out, 0x12345678u));
    }

    {
        std::ifstream in(path, std::ios::binary);
        ASSERT_TRUE(in.is_open());

        auto value = kvdb::endian::read_u32_le(in);

        ASSERT_TRUE(value.has_value());
        EXPECT_EQ(*value, 0x12345678u);
    }

    std::remove(path.c_str());
}

TEST(InternalRecordIOTest, RoundTripsMultipleRecords)
{
    const std::string path = "tmp_internal_record_roundtrip.bin";

    {
        Arena arena;
        InternalRecord record1, record2;

        record1.key_entry = ArenaEntry::make_entry(arena, "key1");
        record1.value_entry = ArenaEntry::make_entry(arena, "value1");
        record1.seq_num = 1LL;
        record1.type = Type::Put;

        record2.key_entry = ArenaEntry::make_entry(arena, "key2");
        record2.value_entry = ArenaEntry::make_entry(arena, "value2");
        record2.seq_num = 2LL;
        record2.type = Type::Tombstone;

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open());

        ASSERT_TRUE(record1.write(out));
        ASSERT_TRUE(record2.write(out));

        out.close();
    }

    Arena arena;
    InternalRecord record1, record2;

    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.is_open());

    auto res1 = InternalRecord::read(in, arena);
    ASSERT_TRUE(res1);
    record1 = std::move(*res1);

    auto res2 = InternalRecord::read(in, arena);
    ASSERT_TRUE(res2);
    record2 = std::move(*res2);

    auto res3 = InternalRecord::read(in, arena);
    EXPECT_FALSE(res3);

    EXPECT_EQ(record1.seq_num, 1LL);
    EXPECT_EQ(record1.type, Type::Put);
    EXPECT_EQ(std::string_view(
        reinterpret_cast<const char*>(record1.key_entry.data),
        record1.key_entry.size
    ), "key1");
    EXPECT_EQ(std::string_view(
        reinterpret_cast<const char*>(record1.value_entry.data),
        record1.value_entry.size
    ), "value1");

    EXPECT_EQ(record2.seq_num, 2LL);
    EXPECT_EQ(record2.type, Type::Tombstone);
    EXPECT_EQ(std::string_view(
        reinterpret_cast<const char*>(record2.key_entry.data),
        record2.key_entry.size
    ), "key2");
    EXPECT_EQ(std::string_view(
        reinterpret_cast<const char*>(record2.value_entry.data),
        record2.value_entry.size
    ), "value2");

    in.close();
    std::remove(path.c_str());
}