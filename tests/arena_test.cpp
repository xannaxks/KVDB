#include <gtest/gtest.h>
#include "arena.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <typeindex>
#include <set>
#include <tuple>
#include <algorithm>
#include <limits>

TEST(ArenaTest, AllocReturnsNonNullForValidSmallAllocation)
{
	Arena arena(1024, 512);
	void* p = arena.alloc(16, alignof(std::max_align_t));

	EXPECT_NE(p, nullptr);
	EXPECT_EQ(arena.get_pages_size(), 1);
	EXPECT_EQ(arena.get_large_size(), 0);
	EXPECT_GE(arena.get_used_bytes(), 16);
	EXPECT_GE(arena.get_reserved_bytes(), 1024);
}

TEST(ArenaTest, AllocZeroSizeReturnsNull)
{
	Arena arena(1024, 512);
	void* p = arena.alloc(0, alignof(std::max_align_t));

	EXPECT_EQ(p, nullptr);
	EXPECT_EQ(arena.get_pages_size(), 0);
	EXPECT_EQ(arena.get_large_size(), 0);
	EXPECT_EQ(arena.get_used_bytes(), 0);
	EXPECT_EQ(arena.get_reserved_bytes(), 0);
}

TEST(ArenaTest, ReturnedPointerProperlyAligned)
{
	Arena arena(1024, 512);
	void* p1 = arena.alloc(1, alignof(std::byte));
	void* p2 = arena.alloc(8, alignof(std::uint64_t));
	void* p3 = arena.alloc(4, alignof(std::uint32_t));

	EXPECT_NE(p1, nullptr);
	EXPECT_NE(p2, nullptr);
	EXPECT_NE(p3, nullptr);

	auto addr1 = reinterpret_cast<std::uintptr_t>(p2);
	auto addr2 = reinterpret_cast<std::uintptr_t>(p1);
	auto addr3 = reinterpret_cast<std::uintptr_t>(p3);
	EXPECT_EQ(addr1 % alignof(uint64_t), 0);
	EXPECT_EQ(addr2 % alignof(std::byte), 0);
	EXPECT_EQ(addr3 % alignof(uint32_t), 0);
}

TEST(ArenaTest, AllocatedMemoryIsWritable)
{
	Arena arena(1024, 512);

	auto* p = static_cast<std::uint32_t*>(
		arena.alloc(sizeof(std::uint32_t), alignof(std::uint32_t))
		);

	ASSERT_NE(p, nullptr);

	*p = 123456;

	EXPECT_EQ(*p, 123456u);
}

TEST(ArenaTest, MultipleSmallAllocationsUseSamePageWhenTheyFit)
{
	Arena arena(1024, 512);

	arena.alloc(100, alignof(std::max_align_t));
	arena.alloc(100, alignof(std::max_align_t));
	arena.alloc(100, alignof(std::max_align_t));

	EXPECT_EQ(arena.get_pages_size(), 1);
	EXPECT_EQ(arena.get_large_size(), 0);
}

TEST(ArenaTest, CreatesNewPageWhenCurrentPageDoesNotFit)
{
	Arena arena(128, 1024);
	arena.alloc(100, alignof(std::max_align_t));
	arena.alloc(100, alignof(std::max_align_t));

	EXPECT_EQ(arena.get_pages_size(), 2);
	EXPECT_EQ(arena.get_large_size(), 0);
}

TEST(ArenaTest, LargeAllocationGoesToLargeList)
{
	Arena arena(1024, 512);
	void* p = arena.alloc(512, alignof(std::max_align_t));

	EXPECT_NE(p, nullptr);
	EXPECT_EQ(arena.get_large_size(), 1);
	EXPECT_EQ(arena.get_pages_size(), 0);
	EXPECT_GE(arena.get_used_bytes(), 512);
	EXPECT_GE(arena.get_reserved_bytes(), 512);
}

TEST(ArenaTest, AllocationBelowLargeThresholdUsesSmallPage)
{
	Arena arena(1024, 512);

	arena.alloc(511, alignof(std::max_align_t));

	EXPECT_EQ(arena.get_pages_size(), 1);
	EXPECT_EQ(arena.get_large_size(), 0);
}

TEST(ArenaTest, AllocationAtLargeThresholdUsesLargeList)
{
	Arena arena(1024, 512);

	arena.alloc(512, alignof(std::max_align_t));

	EXPECT_EQ(arena.get_pages_size(), 0);
	EXPECT_EQ(arena.get_large_size(), 1);
}

TEST(ArenaTest, CheckpointRollbackRestoresUsedBytes)
{
	Arena arena(512, 512);
	void* p = arena.alloc(100, alignof(std::max_align_t));

	EXPECT_NE(p, nullptr);
	auto cp = arena.checkpoint();
	
	uint64_t used_before = arena.get_used_bytes();

	arena.alloc(200, alignof(std::max_align_t));
	EXPECT_GT(arena.get_used_bytes(), used_before);

	arena.rollback(cp);

	EXPECT_EQ(arena.get_used_bytes(), used_before);
}

TEST(ArenaTest, RollbackRemovesPagesAllocatedAfterCheckpoint)
{
	Arena arena(128, 512);
	arena.alloc(100, alignof(std::max_align_t));
	
	auto cp = arena.checkpoint();
	
	arena.alloc(100, alignof(std::max_align_t));

	EXPECT_EQ(arena.get_pages_size(), 2);

	arena.rollback(cp);

	EXPECT_EQ(arena.get_pages_size(), 1);
}

TEST(ArenaTest, RollbackRemovesLargeAllocationsAfterCheckpoint)
{
	Arena arena(128, 512);
	arena.alloc(100, alignof(std::max_align_t));

	auto cp = arena.checkpoint();

	arena.alloc(1000, alignof(std::max_align_t));

	EXPECT_EQ(arena.get_pages_size(), 1);
	EXPECT_EQ(arena.get_large_size(), 1);

	arena.rollback(cp);;

	EXPECT_EQ(arena.get_pages_size(), 1);
	EXPECT_EQ(arena.get_large_size(), 0);
}

TEST(ArenaTest, RollbackAllowsMemoryReuse)
{
	Arena arena(1024, 512);

	void* p1 = arena.alloc(100, alignof(std::max_align_t));
	auto cp = arena.checkpoint();

	void* p2 = arena.alloc(100, alignof(std::max_align_t));
	arena.rollback(cp);

	void* p3 = arena.alloc(100, alignof(std::max_align_t));

	EXPECT_EQ(p2, p3);
	EXPECT_NE(p1, nullptr);
}

TEST(ArenaTest, ResetFalseKeepsFirstPageButClearsEverythingElse)
{
	Arena arena(128, 512);

	arena.alloc(100, alignof(std::max_align_t));
	arena.alloc(100, alignof(std::max_align_t));
	arena.alloc(512, alignof(std::max_align_t));

	EXPECT_EQ(arena.get_pages_size(), 2);
	EXPECT_EQ(arena.get_large_size(), 1);

	arena.reset(false);

	EXPECT_EQ(arena.get_pages_size(), 1);
	EXPECT_EQ(arena.get_large_size(), 0);
	EXPECT_EQ(arena.get_used_bytes(), 0);
}

TEST(ArenaTest, ResetTrueReleasesEverything)
{
	Arena arena(128, 512);

	arena.alloc(100, alignof(std::max_align_t));
	arena.alloc(100, alignof(std::max_align_t));
	arena.alloc(512, alignof(std::max_align_t));

	EXPECT_EQ(arena.get_pages_size(), 2);
	EXPECT_EQ(arena.get_large_size(), 1);

	arena.reset(true);

	EXPECT_EQ(arena.get_pages_size(), 0);
	EXPECT_EQ(arena.get_reserved_bytes(), 0);
	EXPECT_EQ(arena.get_used_bytes(), 0);
	EXPECT_EQ(arena.get_large_size(), 0);
}

TEST(ArenaTest, ArenaCopyBytesCopiesData)
{
	Arena arena(1024, 512);

	const char* text = "Hello, Arena!";
	ArenaEntry span = arena_copy_bytes(arena, text, 13);

	ASSERT_NE(span.data, nullptr);
	EXPECT_EQ(span.size, 13u);
	EXPECT_EQ(std::memcmp(span.data, text, 13), 0);
}

TEST(ArenaTest, ArenaCopyBytesZeroSizeReturnsNullSpan)
{
	Arena arena(1024, 512);

	const char* text = "zero";
	ArenaEntry span = arena_copy_bytes(arena, text, 0);

	EXPECT_EQ(span.data, nullptr);
	EXPECT_EQ(span.size, 0u);
}

TEST(ArenaTest, EqualEntriesCompareEqual)
{
	char a[] = "abc";
	char b[] = "abc";

	ArenaEntry e1(a, 3);
	ArenaEntry e2(b, 3);

	EXPECT_TRUE(e1 == e2);
	EXPECT_FALSE(e1 < e2);
	EXPECT_FALSE(e1 > e2);
}

TEST(ArenaEntryTest, LexicographicComparisonWorks)
{
	char a[] = "abc";
	char b[] = "abd";

	ArenaEntry e1(a, 3);
	ArenaEntry e2(b, 3);

	EXPECT_TRUE(e1 < e2);
	EXPECT_TRUE(e2 > e1);
	EXPECT_FALSE(e1 == e2);
}

TEST(ArenaEntryTest, ShorterPrefixIsSmaller)
{
	char a[] = "abc";
	char b[] = "abcd";

	ArenaEntry e1(a, 3);
	ArenaEntry e2(b, 4);

	EXPECT_TRUE(e1 < e2);
	EXPECT_TRUE(e2 > e1);
	EXPECT_FALSE(e1 == e2);
}

TEST(ArenaEntryTest, BinaryDataComparisonWorks)
{
	unsigned char a[] = { 0x00, 0x10, 0xFF };
	unsigned char b[] = { 0x00, 0x20, 0x01 };

	ArenaEntry e1(a, 3);
	ArenaEntry e2(b, 3);

	EXPECT_TRUE(e1 < e2);
	EXPECT_FALSE(e1 == e2);
}

TEST(ArenaDeathTest, ConstructorRejectsZeroPageSize)
{
	EXPECT_DEATH({ Arena arena(0, 1024); }, "");
}

TEST(ArenaDeathTest, ConstructorRejectsZeroLargeThreshold)
{
	EXPECT_DEATH({ Arena arena(1024, 0); }, "");
}

TEST(ArenaDeathTest, AllocRejectsInvalidAlignment)
{
	Arena arena(1024, 512);

	EXPECT_DEATH({ arena.alloc(16, 3); }, "");
}

TEST(ArenaEntryTest, DefaultConstructorCreatesEmptyEntry)
{
	ArenaEntry entry;

	EXPECT_EQ(entry.data, nullptr);
	EXPECT_EQ(entry.size, 0u);
}

TEST(ArenaEntryTest, ConstructorStoresPointerAndSize)
{
	char buffer[] = "abc";

	ArenaEntry entry(buffer, 3);

	EXPECT_EQ(entry.data, buffer);
	EXPECT_EQ(entry.size, 3u);
}

TEST(ArenaEntryTest, EmptyEntriesAreEqual)
{
	ArenaEntry a;
	ArenaEntry b;

	EXPECT_TRUE(a == b);
	EXPECT_FALSE(a < b);
	EXPECT_FALSE(a > b);
}

TEST(ArenaEntryTest, EqualBytesAreEqual)
{
	Arena arena;

	ArenaEntry a = ArenaEntry::make_entry(arena, std::string("hello"));
	ArenaEntry b = ArenaEntry::make_entry(arena, std::string("hello"));

	EXPECT_TRUE(a == b);
	EXPECT_FALSE(a < b);
	EXPECT_FALSE(a > b);
}

TEST(ArenaEntryTest, DifferentBytesAreNotEqual)
{
	Arena arena;

	ArenaEntry a = ArenaEntry::make_entry(arena, std::string("hello"));
	ArenaEntry b = ArenaEntry::make_entry(arena, std::string("world"));

	EXPECT_FALSE(a == b);
}

TEST(ArenaEntryTest, LexicographicLessWorks)
{
	Arena arena;

	ArenaEntry a = ArenaEntry::make_entry(arena, std::string("abc"));
	ArenaEntry b = ArenaEntry::make_entry(arena, std::string("abd"));

	EXPECT_TRUE(a < b);
	EXPECT_FALSE(b < a);
	EXPECT_TRUE(b > a);
}

TEST(ArenaEntryTest, PrefixShorterStringIsLess)
{
	Arena arena;

	ArenaEntry a = ArenaEntry::make_entry(arena, std::string("abc"));
	ArenaEntry b = ArenaEntry::make_entry(arena, std::string("abcd"));

	EXPECT_TRUE(a < b);
	EXPECT_FALSE(b < a);
	EXPECT_TRUE(b > a);
}

TEST(ArenaEntryTest, EmptyEntryIsLessThanNonEmptyEntry)
{
	Arena arena;

	ArenaEntry empty;
	ArenaEntry non_empty = ArenaEntry::make_entry(arena, std::string("a"));

	EXPECT_TRUE(empty < non_empty);
	EXPECT_FALSE(non_empty < empty);
	EXPECT_TRUE(non_empty > empty);
}

TEST(ArenaEntryTest, MakeEntryCopiesStringDataIntoArena)
{
	Arena arena;

	std::string s = "hello";
	ArenaEntry entry = ArenaEntry::make_entry(arena, s);

	ASSERT_NE(entry.data, nullptr);
	EXPECT_EQ(entry.size, 5u);
	EXPECT_EQ(std::memcmp(entry.data, "hello", 5), 0);

	s[0] = 'X';

	EXPECT_EQ(std::memcmp(entry.data, "hello", 5), 0);
}

TEST(ArenaEntryTest, MakeEntrySupportsEmbeddedNullBytes)
{
	Arena arena;

	std::string s;
	s.push_back('a');
	s.push_back('\0');
	s.push_back('b');

	ArenaEntry entry = ArenaEntry::make_entry(arena, s);

	ASSERT_NE(entry.data, nullptr);
	EXPECT_EQ(entry.size, 3u);

	const char expected[] = { 'a', '\0', 'b' };
	EXPECT_EQ(std::memcmp(entry.data, expected, 3), 0);
}

TEST(ArenaEntryTest, ArenaCopyBytesCopiesRawBytes)
{
	Arena arena;

	const unsigned char src[] = { 0x00, 0x01, 0x7F, 0xFF };

	ArenaEntry entry = arena_copy_bytes(arena, src, 4);

	ASSERT_NE(entry.data, nullptr);
	EXPECT_EQ(entry.size, 4u);
	EXPECT_EQ(std::memcmp(entry.data, src, 4), 0);
}

TEST(ArenaEntryTest, ArenaCopyBytesWithZeroSizeReturnsEmptyEntry)
{
	Arena arena;

	ArenaEntry entry = arena_copy_bytes(arena, nullptr, 0);

	EXPECT_EQ(entry.data, nullptr);
	EXPECT_EQ(entry.size, 0u);
}

TEST(ArenaEntryTest, MoveConstructorTransfersPointerAndClearsSource)
{
	Arena arena;

	ArenaEntry original = ArenaEntry::make_entry(arena, std::string("hello"));

	void* old_ptr = original.data;
	std::uint32_t old_size = original.size;

	ArenaEntry moved(std::move(original));

	EXPECT_EQ(moved.data, old_ptr);
	EXPECT_EQ(moved.size, old_size);

	EXPECT_EQ(original.data, nullptr);
	EXPECT_EQ(original.size, 0u);
}

TEST(ArenaEntryTest, MoveAssignmentTransfersPointerAndClearsSource)
{
	Arena arena;

	ArenaEntry source = ArenaEntry::make_entry(arena, std::string("hello"));
	ArenaEntry target = ArenaEntry::make_entry(arena, std::string("bye"));

	void* old_ptr = source.data;
	std::uint32_t old_size = source.size;

	target = std::move(source);

	EXPECT_EQ(target.data, old_ptr);
	EXPECT_EQ(target.size, old_size);

	EXPECT_EQ(source.data, nullptr);
	EXPECT_EQ(source.size, 0u);
}

TEST(ArenaEntryTest, SelfMoveAssignmentDoesNotDestroyEntry)
{
	Arena arena;

	ArenaEntry entry = ArenaEntry::make_entry(arena, std::string("hello"));

	void* old_ptr = entry.data;
	std::uint32_t old_size = entry.size;

	entry = std::move(entry);

	EXPECT_EQ(entry.data, old_ptr);
	EXPECT_EQ(entry.size, old_size);
	EXPECT_EQ(std::memcmp(entry.data, "hello", 5), 0);
}

TEST(ArenaEntryTest, CopyAssignmentIsShallowHandleCopy)
{
	Arena arena;

	ArenaEntry a = ArenaEntry::make_entry(arena, std::string("hello"));
	ArenaEntry b;

	b = a;

	EXPECT_EQ(b.data, a.data);
	EXPECT_EQ(b.size, a.size);
	EXPECT_TRUE(b == a);
}

TEST(ArenaEntryTest, SortsLexicographically)
{
	Arena arena;

	std::vector<ArenaEntry> entries;
	entries.push_back(ArenaEntry::make_entry(arena, std::string("banana")));
	entries.push_back(ArenaEntry::make_entry(arena, std::string("apple")));
	entries.push_back(ArenaEntry::make_entry(arena, std::string("app")));
	entries.push_back(ArenaEntry::make_entry(arena, std::string("car")));

	std::sort(entries.begin(), entries.end());

	auto as_string = [](const ArenaEntry& e)
		{
			return std::string(static_cast<const char*>(e.data), e.size);
		};

	EXPECT_EQ(as_string(entries[0]), "app");
	EXPECT_EQ(as_string(entries[1]), "apple");
	EXPECT_EQ(as_string(entries[2]), "banana");
	EXPECT_EQ(as_string(entries[3]), "car");
}

TEST(ArenaTest, AllocationOfDifferentAlignments)
{
	Arena arena;
	std::map<void*, int> mp;
	for (int i = 0; i < 10; i++)
	{
		int alignment = std::min(static_cast<int>(alignof(std::max_align_t)), 1 << i);
		void* p = arena.alloc(1, alignment);
		mp[p] = alignment;
	}
	for (auto& [ptr, alignment] : mp)
	{
		auto addr = reinterpret_cast<std::uintptr_t>(ptr);
		ASSERT_EQ(addr % alignment, 0);
	}
}

TEST(ArenaTest, RandomAllocations)
{

	auto random_string = [](int size)
	{
		std::string res;
		while (size--)
		{
			res += static_cast<char>(rand() % 256);
		}
		return res;
	};

	Arena arena(1024, 1024);
	
	std::vector<std::tuple<void*, int, int, std::string>> allocs;
	std::set<void*> seen;
	int large_cnt = 0;

	for (int i = 0; i < 1024; i++)
	{
		int size = rand() % 2048;

		if (size == 0) continue;

		int alignment = 1 << (rand() % 10);
		alignment = std::min(static_cast<int>(alignof(std::max_align_t)), alignment);

		void* ptr = arena.alloc(size, alignment);

		ASSERT_NE(ptr, nullptr);
		ASSERT_TRUE(seen.insert(ptr).second) << "Allocator returned duplicate pointer";

		std::string str = std::move(random_string(size));
		memcpy(ptr, str.data(), size);

		allocs.push_back({ptr, size, alignment, std::move(str)});
		large_cnt += (size >= 1024);
	}

	for (auto& [ptr, size, alignment, str] : allocs)
	{
		auto addr = reinterpret_cast<std::uintptr_t>(ptr);
		EXPECT_EQ(addr % alignment, 0);
		EXPECT_EQ(std::memcmp(ptr, str.data(), size), 0);
	}

	EXPECT_GE(arena.get_large_size(), large_cnt);
	EXPECT_GE(arena.get_pages_size(), 1);
}

TEST(ArenaTest, ConstructTest)
{
	struct Test
	{
		Test() = default;
		Test(const Test& test)
			: bool_(test.bool_), int_(test.int_)
		{
		}
		Test(bool bool_, uint32_t int_)
			: bool_(bool_), int_(int_)
		{
		}

		bool operator==(const Test& other) const
		{
			return bool_ == other.bool_ && int_ == other.int_;
		}

		bool bool_;
		uint32_t int_;
	};

	Arena arena;

	const char char_mock = 'x';
	const int32_t int32_t_mock = 123456;
	const int64_t int64_t_mock = 1234567890123456;
	const double double_mock = 1.23456;
	const Test test_mock{ true, 123456 };

	std::vector<std::pair<char*, int>> char_allocs;
	std::vector<std::pair<int32_t*, int>> int32_t_allocs;
	std::vector<std::pair<int64_t*, int>> int64_t_allocs;
	std::vector<std::pair<double*, int>> double_allocs;
	std::vector<std::pair<Test*, int>> test_allocs;

	char char_mocks[101];
	int32_t int32_t_mocks[101];
	int64_t int64_t_mocks[101];
	double double_mocks[101];
	Test test_mocks[101];
	
	for (int i = 0; i < 101; i++)
	{	
		char_mocks[i] = char_mock;
		int32_t_mocks[i] = int32_t_mock;
		int64_t_mocks[i] = int64_t_mock;
		double_mocks[i] = double_mock;
		test_mocks[i] = test_mock;
	}


	for (int i = 0; i < 100; i++)
	{
		int type = rand() % 5;
		int is_array = rand() % 2;
		const int array_sz = rand() % 10 + 1;
		

		if (type == 0)
		{
			char* ptr;

			if (!is_array)
				ptr = arena_new<char>(arena, char_mock);
			else
			{
				ptr = arena_new_array<char>(arena, array_sz); // not using arean_copy_bytes, cuz this is constructor test
				memcpy(ptr, char_mocks, array_sz * sizeof(char));
			}

			char_allocs.push_back(std::make_pair(ptr, is_array ? array_sz : 1));
		}
		else if (type == 1)
		{
			int32_t* ptr;

			if (!is_array)
				ptr = arena_new<int32_t>(arena, int32_t_mock);
			else
			{
				ptr = arena_new_array<int32_t>(arena, array_sz); // not using arean_copy_bytes, cuz this is constructor test
				memcpy(ptr, int32_t_mocks, array_sz * sizeof(int32_t));
			}

			int32_t_allocs.push_back(std::make_pair(ptr, is_array ? array_sz : 1));
		}
		else if (type == 2)
		{
			int64_t* ptr;

			if (!is_array)
				ptr = arena_new<int64_t>(arena, int64_t_mock);
			else
			{
				ptr = arena_new_array<int64_t>(arena, array_sz);
				memcpy(ptr, int64_t_mocks, array_sz * sizeof(int64_t));
			}

			int64_t_allocs.push_back(std::make_pair(ptr, is_array ? array_sz : 1));
		}
		else if (type == 3)
		{
			double* ptr;

			if (!is_array)
				ptr = arena_new<double>(arena, double_mock);
			else {
				ptr = arena_new_array<double>(arena, array_sz);
				memcpy(ptr, double_mocks, array_sz * sizeof(double));
			}

			double_allocs.push_back(std::make_pair(ptr, is_array ? array_sz : 1));
		}
		else
		{
			Test* ptr;

			if (!is_array)
				ptr = arena_new<Test>(arena, test_mock);
			else
			{
				ptr = arena_new_array<Test>(arena, array_sz);
				memcpy(ptr, test_mocks, array_sz * sizeof(Test));
			}

			test_allocs.push_back(std::make_pair(ptr, is_array ? array_sz : 1));
		}
	}

	for(auto& [ptr, size] : char_allocs)
		ASSERT_EQ(*ptr, char_mock);

	for (auto& [ptr, size] : int32_t_allocs)
		ASSERT_EQ(*ptr, int32_t_mock);

	for (auto& [ptr, size] : int64_t_allocs)
		ASSERT_EQ(*ptr, int64_t_mock);

	for (auto& [ptr, size] : double_allocs)
		ASSERT_EQ(*ptr, double_mock);

	for (int i = 0; i < test_allocs.size(); i++)
	{
		auto& [ptr, size] = test_allocs[i];
		for (int j = 0; j < size; j++)
			ASSERT_EQ(ptr[j], test_mocks[j]);
	}
}

TEST(ArenaTest, NewFollowedByRest)
{
	Arena arena;
	arena_new<char>(arena, 'x');
	arena_new_array<char>(arena, 100);
	arena_new<float>(arena, 1.1);
	arena_new<std::byte>(arena);

	arena.reset(false);

	EXPECT_EQ(arena.get_pages_size(), 1);
	EXPECT_EQ(arena.get_large_size(), 0);

	arena.reset(true);

	EXPECT_EQ(arena.get_pages_size(), 0);
	EXPECT_EQ(arena.get_large_size(), 0);

	char* ptr = arena_new<char>(arena, 'x');

	EXPECT_EQ(arena.get_pages_size(), 1);
	EXPECT_EQ(arena.get_large_size(), 0);

	EXPECT_EQ(*ptr, 'x');
}

TEST(ArenaTest, OldAllocsStableAfterPageGrowth)
{
	Arena arena(128, 512);

	std::vector<std::pair<void*, unsigned char>> ptrs;

	for (int i = 0; i < 10; i++)
	{
		void* p = arena.alloc(100, alignof(std::max_align_t));
		ASSERT_NE(p, nullptr);

		unsigned char value = static_cast<unsigned char>(i + 1);
		std::memset(p, value, 100);

		ptrs.push_back({ p, value });
	}

	for (auto& [p, value] : ptrs)
	{
		unsigned char* bytes = static_cast<unsigned char*>(p);

		for (int i = 0; i < 100; i++)
			EXPECT_EQ(bytes[i], value);
	}
}