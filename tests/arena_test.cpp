#include <gtest/gtest.h>
#include "arena.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    struct LifetimeCounter
    {
        static inline int constructions = 0;
        static inline int destructions = 0;
        static inline int alive = 0;

        int value = 0;

        LifetimeCounter() noexcept
        {
            ++constructions;
            ++alive;
        }

        explicit LifetimeCounter(int initial_value) noexcept
            : value(initial_value)
        {
            ++constructions;
            ++alive;
        }

        LifetimeCounter(const LifetimeCounter&) = delete;
        LifetimeCounter& operator=(const LifetimeCounter&) = delete;

        ~LifetimeCounter() noexcept
        {
            ++destructions;
            --alive;
        }

        static void clear() noexcept
        {
            constructions = 0;
            destructions = 0;
            alive = 0;
        }
    };

    struct DestructionOrder
    {
        std::vector<int>* order = nullptr;
        int id = 0;

        DestructionOrder(std::vector<int>& output, int object_id) noexcept
            : order(&output), id(object_id)
        {
        }

        ~DestructionOrder() noexcept
        {
            order->push_back(id);
        }
    };

    struct ThrowingDefaultConstructor
    {
        static inline int attempts = 0;
        static inline int alive = 0;
        static inline int throw_on_attempt = 0;

        ThrowingDefaultConstructor()
        {
            const int current_attempt = ++attempts;
            if (current_attempt == throw_on_attempt)
            {
                throw std::runtime_error("intentional constructor failure");
            }

            ++alive;
        }

        ~ThrowingDefaultConstructor() noexcept
        {
            --alive;
        }

        static void clear() noexcept
        {
            attempts = 0;
            alive = 0;
            throw_on_attempt = 0;
        }
    };

    struct ThrowingSingleConstructor
    {
        ThrowingSingleConstructor()
        {
            throw std::runtime_error("intentional constructor failure");
        }

        ~ThrowingSingleConstructor() noexcept = default;
    };

    struct Immovable
    {
        int value = 0;

        explicit Immovable(int initial_value) noexcept
            : value(initial_value)
        {
        }

        Immovable(const Immovable&) = delete;
        Immovable& operator=(const Immovable&) = delete;
        Immovable(Immovable&&) = delete;
        Immovable& operator=(Immovable&&) = delete;

        ~Immovable() noexcept = default;
    };

    struct alignas(64) OverAligned
    {
        std::uint64_t value = 123;
    };

    std::uint64_t used_bytes(const Arena& arena)
    {
        Result<std::uint64_t> result = arena.get_used_bytes();
        EXPECT_TRUE(result.is_ok());
        return result.value;
    }

    std::uint64_t reserved_bytes(const Arena& arena)
    {
        Result<std::uint64_t> result = arena.get_reserved_bytes();
        EXPECT_TRUE(result.is_ok());
        return result.value;
    }
} // namespace

TEST(ArenaConstructorTest, RejectsZeroPageSize)
{
    EXPECT_THROW((Arena{ 0, 128 }), std::invalid_argument);
}

TEST(ArenaConstructorTest, RejectsZeroLargeThreshold)
{
    EXPECT_THROW((Arena{ 128, 0 }), std::invalid_argument);
}

TEST(ArenaConstructorTest, RejectsPageSizeAboveNormalPageLimit)
{
    EXPECT_THROW((Arena{ (1u << 20) + 1u, 128 }), std::invalid_argument);
}

TEST(ArenaAllocationTest, ZeroSizeReturnsSuccessfulNullAllocation)
{
    Arena arena(128, 64);

    Result<void*> result = arena.alloc(0, alignof(std::max_align_t));

    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value, nullptr);
    EXPECT_EQ(arena.get_pages_size(), 0u);
    EXPECT_EQ(arena.get_large_size(), 0u);
    EXPECT_EQ(used_bytes(arena), 0u);
    EXPECT_EQ(reserved_bytes(arena), 0u);
}

TEST(ArenaAllocationTest, RejectsZeroAlignment)
{
    Arena arena(128, 64);

    Result<void*> result = arena.alloc(8, 0);

    EXPECT_FALSE(result.is_ok());
}

TEST(ArenaAllocationTest, RejectsNonPowerOfTwoAlignment)
{
    Arena arena(128, 64);

    Result<void*> result = arena.alloc(8, 3);

    EXPECT_FALSE(result.is_ok());
}

TEST(ArenaAllocationTest, ReturnedPointersHaveRequestedAlignment)
{
    Arena arena(256, 128);

    for (std::size_t alignment : {1u, 2u, 4u, 8u, 16u})
    {
        Result<void*> result = arena.alloc(3, alignment);
        ASSERT_TRUE(result.is_ok());
        ASSERT_NE(result.value, nullptr);

        const auto address = reinterpret_cast<std::uintptr_t>(result.value);
        EXPECT_EQ(address % alignment, 0u);
    }
}

TEST(ArenaAllocationTest, OverAlignedAllocationUsesDedicatedBlock)
{
    Arena arena(256, 128);

    Result<OverAligned*> result = arena_new<OverAligned>(arena);

    ASSERT_TRUE(result.is_ok());
    ASSERT_NE(result.value, nullptr);
    EXPECT_EQ(
        reinterpret_cast<std::uintptr_t>(result.value) % alignof(OverAligned),
        0u
    );
    EXPECT_EQ(arena.get_large_size(), 1u);
}

TEST(ArenaAllocationTest, SmallAllocationsShareAPageWhenTheyFit)
{
    Arena arena(256, 128);

    ASSERT_TRUE(arena.alloc(40, 8).is_ok());
    ASSERT_TRUE(arena.alloc(40, 8).is_ok());
    ASSERT_TRUE(arena.alloc(40, 8).is_ok());

    EXPECT_EQ(arena.get_pages_size(), 1u);
    EXPECT_EQ(arena.get_large_size(), 0u);
}

TEST(ArenaAllocationTest, CreatesAnotherPageWhenCurrentPageIsFull)
{
    Arena arena(128, 512);

    ASSERT_TRUE(arena.alloc(100, alignof(std::max_align_t)).is_ok());
    ASSERT_TRUE(arena.alloc(100, alignof(std::max_align_t)).is_ok());

    EXPECT_EQ(arena.get_pages_size(), 2u);
    EXPECT_EQ(arena.get_large_size(), 0u);
}

TEST(ArenaAllocationTest, ThresholdAllocationUsesLargeList)
{
    Arena arena(1024, 512);

    Result<void*> result = arena.alloc(512, alignof(std::max_align_t));

    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(arena.get_pages_size(), 0u);
    EXPECT_EQ(arena.get_large_size(), 1u);
    EXPECT_EQ(used_bytes(arena), 512u);
    EXPECT_EQ(reserved_bytes(arena), 512u);
}

TEST(ArenaAllocationTest, AllocationLargerThanNormalPageLimitUsesLargeList)
{
    Arena arena(128, std::numeric_limits<std::size_t>::max());

    Result<void*> result = arena.alloc(
        (1u << 20) + 1u,
        alignof(std::max_align_t)
    );

    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(arena.get_pages_size(), 0u);
    EXPECT_EQ(arena.get_large_size(), 1u);
}

TEST(ArenaAllocationTest, RawStorageIsWritable)
{
    Arena arena(128, 64);
    Result<void*> result = arena.alloc(32, alignof(std::byte));
    ASSERT_TRUE(result.is_ok());

    std::memset(result.value, 0x5A, 32);

    const auto* bytes = static_cast<const unsigned char*>(result.value);
    for (std::size_t i = 0; i < 32; ++i)
    {
        EXPECT_EQ(bytes[i], 0x5A);
    }
}

TEST(ArenaAllocationTest, ExistingAllocationsStayStableAfterPageGrowth)
{
    Arena arena(128, 512);
    std::vector<std::pair<void*, unsigned char>> allocations;

    for (int i = 0; i < 10; ++i)
    {
        Result<void*> result = arena.alloc(100, alignof(std::max_align_t));
        ASSERT_TRUE(result.is_ok());

        const unsigned char value = static_cast<unsigned char>(i + 1);
        std::memset(result.value, value, 100);
        allocations.emplace_back(result.value, value);
    }

    for (const auto& [memory, expected] : allocations)
    {
        const auto* bytes = static_cast<const unsigned char*>(memory);
        for (std::size_t i = 0; i < 100; ++i)
        {
            EXPECT_EQ(bytes[i], expected);
        }
    }
}

TEST(ArenaConstructionTest, ConstructsImmovableTypeDirectlyInArena)
{
    Arena arena;

    Result<Immovable*> result = arena_new<Immovable>(arena, 77);

    ASSERT_TRUE(result.is_ok());
    ASSERT_NE(result.value, nullptr);
    EXPECT_EQ(result.value->value, 77);
}

TEST(ArenaConstructionTest, ValueInitializesPrimitiveArray)
{
    Arena arena;

    Result<std::uint32_t*> result = arena_new_array<std::uint32_t>(arena, 8);

    ASSERT_TRUE(result.is_ok());
    for (std::size_t i = 0; i < 8; ++i)
    {
        EXPECT_EQ(result.value[i], 0u);
    }
    EXPECT_EQ(arena.get_destructor_count(), 0u);
}

TEST(ArenaConstructionTest, RejectsZeroLengthArray)
{
    Arena arena;

    Result<int*> result = arena_new_array<int>(arena, 0);

    EXPECT_FALSE(result.is_ok());
    EXPECT_EQ(used_bytes(arena), 0u);
}

TEST(ArenaConstructionTest, RejectsArrayByteSizeOverflow)
{
    Arena arena;

    Result<std::uint64_t*> result = arena_new_array<std::uint64_t>(
        arena,
        std::numeric_limits<std::size_t>::max()
    );

    EXPECT_FALSE(result.is_ok());
    EXPECT_EQ(used_bytes(arena), 0u);
}

TEST(ArenaLifetimeTest, ResetDestroysSingleNonTrivialObject)
{
    LifetimeCounter::clear();
    Arena arena;

    Result<LifetimeCounter*> result = arena_new<LifetimeCounter>(arena, 42);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value->value, 42);
    EXPECT_EQ(LifetimeCounter::alive, 1);
    EXPECT_EQ(arena.get_destructor_count(), 1u);

    arena.reset(false);

    EXPECT_EQ(LifetimeCounter::alive, 0);
    EXPECT_EQ(LifetimeCounter::destructions, 1);
    EXPECT_EQ(arena.get_destructor_count(), 0u);
}

TEST(ArenaLifetimeTest, ResetDestroysArrayElements)
{
    LifetimeCounter::clear();
    Arena arena;

    Result<LifetimeCounter*> result = arena_new_array<LifetimeCounter>(arena, 5);
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(LifetimeCounter::alive, 5);
    EXPECT_EQ(arena.get_destructor_count(), 5u);

    arena.reset(false);

    EXPECT_EQ(LifetimeCounter::alive, 0);
    EXPECT_EQ(LifetimeCounter::destructions, 5);
}

TEST(ArenaLifetimeTest, ArenaDestructorDestroysOwnedObjects)
{
    LifetimeCounter::clear();

    {
        Arena arena;
        ASSERT_TRUE(arena_new<LifetimeCounter>(arena, 1).is_ok());
        ASSERT_TRUE(arena_new_array<LifetimeCounter>(arena, 3).is_ok());
        EXPECT_EQ(LifetimeCounter::alive, 4);
    }

    EXPECT_EQ(LifetimeCounter::alive, 0);
    EXPECT_EQ(LifetimeCounter::destructions, 4);
}

TEST(ArenaLifetimeTest, DestructionUsesReverseConstructionOrder)
{
    Arena arena;
    std::vector<int> order;

    ASSERT_TRUE(arena_new<DestructionOrder>(arena, order, 1).is_ok());
    ASSERT_TRUE(arena_new<DestructionOrder>(arena, order, 2).is_ok());
    ASSERT_TRUE(arena_new<DestructionOrder>(arena, order, 3).is_ok());

    arena.reset(false);

    EXPECT_EQ(order, (std::vector<int>{3, 2, 1}));
}

TEST(ArenaRollbackTest, RestoresUsedBytesAndReusesStorage)
{
    Arena arena(1024, 512);

    Result<void*> first = arena.alloc(100, alignof(std::max_align_t));
    ASSERT_TRUE(first.is_ok());

    const Arena::Checkpoint cp = arena.checkpoint();
    const std::uint64_t before = used_bytes(arena);

    Result<void*> discarded = arena.alloc(100, alignof(std::max_align_t));
    ASSERT_TRUE(discarded.is_ok());
    EXPECT_GT(used_bytes(arena), before);

    arena.rollback(cp);
    EXPECT_EQ(used_bytes(arena), before);

    Result<void*> reused = arena.alloc(100, alignof(std::max_align_t));
    ASSERT_TRUE(reused.is_ok());
    EXPECT_EQ(reused.value, discarded.value);
}

TEST(ArenaRollbackTest, DestroysOnlyObjectsCreatedAfterCheckpoint)
{
    LifetimeCounter::clear();
    Arena arena;

    Result<LifetimeCounter*> first = arena_new<LifetimeCounter>(arena, 1);
    ASSERT_TRUE(first.is_ok());
    const Arena::Checkpoint cp = arena.checkpoint();

    ASSERT_TRUE(arena_new<LifetimeCounter>(arena, 2).is_ok());
    ASSERT_TRUE(arena_new_array<LifetimeCounter>(arena, 2).is_ok());
    EXPECT_EQ(LifetimeCounter::alive, 4);

    arena.rollback(cp);

    EXPECT_EQ(LifetimeCounter::alive, 1);
    EXPECT_EQ(LifetimeCounter::destructions, 3);
    EXPECT_EQ(first.value->value, 1);

    arena.reset(true);
    EXPECT_EQ(LifetimeCounter::alive, 0);
}

TEST(ArenaRollbackTest, RemovesPagesAndLargeBlocksCreatedAfterCheckpoint)
{
    Arena arena(128, 512);
    ASSERT_TRUE(arena.alloc(100, alignof(std::max_align_t)).is_ok());

    const Arena::Checkpoint cp = arena.checkpoint();

    ASSERT_TRUE(arena.alloc(100, alignof(std::max_align_t)).is_ok());
    ASSERT_TRUE(arena.alloc(1000, alignof(std::max_align_t)).is_ok());
    EXPECT_EQ(arena.get_pages_size(), 2u);
    EXPECT_EQ(arena.get_large_size(), 1u);

    arena.rollback(cp);

    EXPECT_EQ(arena.get_pages_size(), 1u);
    EXPECT_EQ(arena.get_large_size(), 0u);
}

TEST(ArenaRollbackTest, RejectsCheckpointFromAnotherArena)
{
    Arena first;
    Arena second;

    const Arena::Checkpoint foreign = first.checkpoint();

    EXPECT_THROW(second.rollback(foreign), std::invalid_argument);
}

TEST(ArenaRollbackTest, RejectsCheckpointThatIsNoLongerReachable)
{
    Arena arena;
    ASSERT_TRUE(arena.alloc(32, 8).is_ok());
    const Arena::Checkpoint cp = arena.checkpoint();

    arena.reset(true);

    EXPECT_THROW(arena.rollback(cp), std::invalid_argument);
}

TEST(ArenaExceptionSafetyTest, FailedSingleConstructionRestoresArenaState)
{
    Arena arena(128, 64);
    const std::uint64_t used_before = used_bytes(arena);
    const std::uint64_t reserved_before = reserved_bytes(arena);

    EXPECT_THROW(
        (void)arena_new<ThrowingSingleConstructor>(arena),
        std::runtime_error
    );

    EXPECT_EQ(used_bytes(arena), used_before);
    EXPECT_EQ(reserved_bytes(arena), reserved_before);
    EXPECT_EQ(arena.get_destructor_count(), 0u);
}

TEST(ArenaExceptionSafetyTest, FailedArrayConstructionDestroysPrefixAndRollsBack)
{
    ThrowingDefaultConstructor::clear();
    ThrowingDefaultConstructor::throw_on_attempt = 3;

    Arena arena(256, 128);
    const std::uint64_t used_before = used_bytes(arena);
    const std::uint64_t reserved_before = reserved_bytes(arena);

    EXPECT_THROW(
        (void)arena_new_array<ThrowingDefaultConstructor>(arena, 5),
        std::runtime_error
    );

    EXPECT_EQ(ThrowingDefaultConstructor::attempts, 3);
    EXPECT_EQ(ThrowingDefaultConstructor::alive, 0);
    EXPECT_EQ(used_bytes(arena), used_before);
    EXPECT_EQ(reserved_bytes(arena), reserved_before);
    EXPECT_EQ(arena.get_destructor_count(), 0u);
}

TEST(ArenaResetTest, ResetFalseKeepsOnlyFirstNormalPage)
{
    Arena arena(128, 512);

    ASSERT_TRUE(arena.alloc(100, alignof(std::max_align_t)).is_ok());
    ASSERT_TRUE(arena.alloc(100, alignof(std::max_align_t)).is_ok());
    ASSERT_TRUE(arena.alloc(512, alignof(std::max_align_t)).is_ok());

    arena.reset(false);

    EXPECT_EQ(arena.get_pages_size(), 1u);
    EXPECT_EQ(arena.get_large_size(), 0u);
    EXPECT_EQ(used_bytes(arena), 0u);
    EXPECT_EQ(reserved_bytes(arena), 128u);
}

TEST(ArenaResetTest, ResetFalseAllowsFirstPageReuse)
{
    Arena arena(128, 512);

    Result<void*> before = arena.alloc(32, 8);
    ASSERT_TRUE(before.is_ok());

    arena.reset(false);

    Result<void*> after = arena.alloc(32, 8);
    ASSERT_TRUE(after.is_ok());
    EXPECT_EQ(after.value, before.value);
}

TEST(ArenaResetTest, ResetTrueReleasesEverything)
{
    Arena arena(128, 512);

    ASSERT_TRUE(arena.alloc(100, alignof(std::max_align_t)).is_ok());
    ASSERT_TRUE(arena.alloc(1000, alignof(std::max_align_t)).is_ok());

    arena.reset(true);

    EXPECT_EQ(arena.get_pages_size(), 0u);
    EXPECT_EQ(arena.get_large_size(), 0u);
    EXPECT_EQ(used_bytes(arena), 0u);
    EXPECT_EQ(reserved_bytes(arena), 0u);
}

TEST(ArenaEntryTest, DefaultEntryIsEmpty)
{
    ArenaEntry entry;

    EXPECT_EQ(entry.data, nullptr);
    EXPECT_EQ(entry.size, 0u);
}

TEST(ArenaEntryTest, RejectsNullPointerForNonEmptyEntry)
{
    EXPECT_THROW((ArenaEntry{ nullptr, 1 }), std::invalid_argument);
}

TEST(ArenaEntryTest, MakeEntryCopiesStringStorage)
{
    Arena arena;
    std::string source = "hello";

    Result<ArenaEntry> result = ArenaEntry::make_entry(arena, source);
    ASSERT_TRUE(result.is_ok());

    source[0] = 'X';

    EXPECT_EQ(result.value.size, 5u);
    EXPECT_EQ(std::memcmp(result.value.data, "hello", 5), 0);
}

TEST(ArenaEntryTest, MakeEntrySupportsEmbeddedNullBytes)
{
    Arena arena;
    const std::string source{ "a\0b", 3 };

    Result<ArenaEntry> result = ArenaEntry::make_entry(arena, source);
    ASSERT_TRUE(result.is_ok());

    const std::array<char, 3> expected{ 'a', '\0', 'b' };
    EXPECT_EQ(result.value.size, 3u);
    EXPECT_EQ(std::memcmp(result.value.data, expected.data(), expected.size()), 0);
}

TEST(ArenaEntryTest, CopyBytesRejectsNullSourceForNonZeroSize)
{
    Arena arena;

    Result<ArenaEntry> result = arena_copy_bytes(arena, nullptr, 4);

    EXPECT_FALSE(result.is_ok());
}

TEST(ArenaEntryTest, EmptyCopyAcceptsNullSource)
{
    Arena arena;

    Result<ArenaEntry> result = arena_copy_bytes(arena, nullptr, 0);

    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value.data, nullptr);
    EXPECT_EQ(result.value.size, 0u);
}

TEST(ArenaEntryTest, ComparesBinaryDataLexicographically)
{
    const std::array<unsigned char, 3> first{ 0x00, 0x10, 0xFF };
    const std::array<unsigned char, 3> second{ 0x00, 0x20, 0x01 };

    const ArenaEntry a{ const_cast<unsigned char*>(first.data()), first.size() };
    const ArenaEntry b{ const_cast<unsigned char*>(second.data()), second.size() };

    EXPECT_TRUE(a < b);
    EXPECT_TRUE(b > a);
    EXPECT_FALSE(a == b);
}

TEST(ArenaEntryTest, ShorterEqualPrefixSortsFirst)
{
    char abc[] = "abc";
    char abcd[] = "abcd";

    const ArenaEntry shorter{ abc, 3 };
    const ArenaEntry longer{ abcd, 4 };

    EXPECT_TRUE(shorter < longer);
    EXPECT_TRUE(shorter <= longer);
    EXPECT_TRUE(longer >= shorter);
}

TEST(ArenaEntryTest, MoveClearsSourceHandle)
{
    Arena arena;
    Result<ArenaEntry> made = ArenaEntry::make_entry(arena, std::string{ "hello" });
    ASSERT_TRUE(made.is_ok());

    ArenaEntry source = made.value;
    const void* old_data = source.data;
    const std::uint32_t old_size = source.size;

    ArenaEntry target = std::move(source);

    EXPECT_EQ(target.data, old_data);
    EXPECT_EQ(target.size, old_size);
    EXPECT_EQ(source.data, nullptr);
    EXPECT_EQ(source.size, 0u);
}