#include <gtest/gtest.h>

TEST(ArenaTest, BasicAllocate)
{
	Arena arena;
	
	void* ptr = arena.alloc(4, 4);
	ASSERT_NE(ptr, nullptr);

	std::memset(ptr, 0xAB, 4);
}
TEST(ArenaTest, AllocationIsProperlyAligned)
{
	Arena arena;

	void* align4 = arena.alloc(5, 4);
	void* align8 = arena.alloc(9, 8);
	void* align_int = arena.alloc(sizeof(int), alignof(int));
	void* align_ll = arena.alloc(sizeof(long long), alignof(long long));

	ASSERT_EQ(reinterpret_cast<uintptr_t>(align4) % 4, 0);
	ASSERT_EQ(reinterpret_cast<uintptr_t>(align8) % 8, 0);
	ASSERT_EQ(reinterpret_cast<uintptr_t>(align_int) % alignof(int), 0);
	ASSERT_EQ(reinterpret_cast<uintptr_t>(align_ll) % alignof(long long), 0);
}
TEST(ArenaTest, AllocationsDontOverlap)
{
	Arena arena;
	std::byte* ptr1 = reinterpret_cast<std::byte*>(arena.alloc(16));
	std::byte* ptr2 = reinterpret_cast<std::byte*>(arena.alloc(16));
	ASSERT_NE(ptr1, nullptr);
	ASSERT_NE(ptr2, nullptr);

	std::memset(ptr1, 0x11, 16);
	std::memset(ptr2, 0x22, 16);

	for (int i = 0; i < 16; i++)
	{
		EXPECT_EQ(ptr1[i], std::byte{ 0x11 });
		EXPECT_EQ(ptr2[i], std::byte{ 0x22 });
	}
}
TEST(ArenaTest, DataSurvivesLaterAllocations)
{
	Arena arena;
	char* char_ptr = reinterpret_cast<char*>(arena.alloc(32));
	ASSERT_NE(char_ptr, nullptr);
	std::memcpy(char_ptr, "hello arena allocator", 22);

	for (int i = 0; i < 20; i++)
	{
		void* temp = arena.alloc(24);
		ASSERT_NE(temp, nullptr);
		std::memset(temp, 0xCD, 24);
	}

	EXPECT_STREQ(char_ptr, "hello arena allocator");
}
TEST(ArenaTest, LargeAllocationsWorkAsIntended)
{
	Arena arena(1, 10);
	void* a = arena.alloc(1);
	void* b = arena.alloc(10);
	ASSERT_NE(a, nullptr);
	ASSERT_NE(b, nullptr);
	ASSERT_EQ(arena.get_pages_size(), 1);
	ASSERT_EQ(arena.get_large_size(), 1);
}

TEST(ArenaTest, CheckpointAndResetWorkability)
{
	Arena arena;
	void* a = arena.alloc(1);
	ASSERT_NE(a, nullptr);
	Arena::CheckPoint cp = arena.checkpoint();
	ASSERT_NE(cp, nullptr);
	ASSERT_NE(cp, Arena::CheckPoint{ 0, 0 });
	void* b = arena.alloc(1);
	ASSERT_NE(b, nullptr);
	ASSERT_EQ(arena.get_pages_size(), 2);
	arena.rollback(cp);
	ASSERT_EQ(arena.get_pages_size(), 1);
}

TEST(ArenaTest, ZeroBytesAllocationBehavior)
{
	Arena arena(20);
	void* p1 = arena.alloc(0);
	void* p2 = arena.alloc(0);

	ASSERT_EQ(p1, nullptr);
	ASSERT_EQ(p2, nullptr);
}

TEST(ArenaTest, BytesRequestedValidity)
{
	Arena arena(128);
	ASSERT_EQ(arena.get_bytes_requested(), 0);

	arena.allocate(10);
	arena.allocate(20);
	arena.allocate(0);
	ASSERT_EQ(arena.get_bytes_requested(), 30);
	arena.reset(true);
	ASSERT_EQ(arena.get_bytes_requested(), 30);
}
