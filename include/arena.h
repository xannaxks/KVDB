/**
 * @file arena.h
 * @brief Page-based region allocation and arena-owned object lifetime support.
 *
 * The arena serves the transient ownership needs of MemTables, manifest
 * recovery, and SSTable decoding. Small allocations are packed into reusable
 * pages, while large or over-aligned allocations receive dedicated pages.
 *
 * Checkpoints provide stack-like rollback. Objects created with arena_new() or
 * arena_new_array() have their destructors invoked in reverse construction
 * order when the arena is rolled back, reset, or destroyed.
 *
 * @note Memory returned by an Arena remains valid only until a rollback that
 *       crosses its allocation, reset(), or destruction of the arena.
 * @note Arena is not thread-safe.
 */
#pragma once
#define NOMINMAX

#include "status.h"

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

class Arena;

template <class T, class... Args>
Result<T*> arena_new(Arena& arena, Args&&... args);

template <class T>
Result<T*> arena_new_array(Arena& arena, std::size_t count);

/**
 * @brief Non-owning byte range whose storage is normally owned by an Arena.
 *
 * ArenaEntry is the common key/value representation used by the storage
 * engine. Comparisons are lexicographic over the referenced bytes.
 */
struct ArenaEntry
{
    void* data = nullptr;
    std::uint32_t size = 0;

    ArenaEntry() noexcept = default;
    ArenaEntry(const void* ptr, std::size_t entry_size);

    ArenaEntry(const ArenaEntry&) noexcept = default;
    ArenaEntry& operator=(const ArenaEntry&) noexcept = default;

    ArenaEntry(ArenaEntry&& other) noexcept;
    ArenaEntry& operator=(ArenaEntry&& other) noexcept;

	std::size_t approximate_memory_usage() const noexcept;

    static std::string generate_random_key(
        const std::string& prefix,
        std::size_t length = 100,
        bool fixed = true
    );

    static std::string generate_random_value(
        const std::string& prefix,
        std::size_t length = 1000,
        bool fixed = true
    );

    [[nodiscard]] bool operator<(const ArenaEntry& other) const noexcept;
    [[nodiscard]] bool operator>(const ArenaEntry& other) const noexcept;
    [[nodiscard]] bool operator==(const ArenaEntry& other) const noexcept;
    [[nodiscard]] bool operator>=(const ArenaEntry& other) const noexcept;
    [[nodiscard]] bool operator<=(const ArenaEntry& other) const noexcept;

    static Result<ArenaEntry> make_entry(
        Arena& arena,
        std::span<const std::byte> bytes
    );

    static Result<ArenaEntry> make_entry(
        Arena& arena,
        const std::string& str
    );
};

// arena entry formatter
template <>
struct std::formatter<ArenaEntry> : std::formatter<std::string_view>
{
    auto format(const ArenaEntry& entry, std::format_context& ctx) const
    {
        const std::string_view view(
            static_cast<const char*>(entry.data),
            entry.size
        );

        return std::formatter<std::string_view>::format(view, ctx);
    }
};

/**
 * @brief Region allocator with checkpoints and optional destructor tracking.
 *
 * Normal allocations grow geometrically sized pages up to an internal cap.
 * Requests at or above the configured large threshold use independent pages
 * so that a single large value does not consume the normal allocation stream.
 *
 * Use alloc() for raw storage and arena_new()/arena_new_array() for objects
 * whose destructors must be managed by the arena.
 */
class Arena
{
private:
    struct Page
    {
        void* base = nullptr;
        std::size_t cap = 0;
        std::size_t used = 0;
        std::size_t alignment = alignof(std::max_align_t);

        Page() noexcept = default;

        Page(
            void* page_base,
            std::size_t page_capacity,
            std::size_t page_used,
            std::size_t page_alignment
        ) noexcept
            : base(page_base),
            cap(page_capacity),
            used(page_used),
            alignment(page_alignment)
        {
        }

        Page(const Page&) = delete;
        Page& operator=(const Page&) = delete;

        Page(Page&& other) noexcept
            : base(std::exchange(other.base, nullptr)),
            cap(std::exchange(other.cap, 0)),
            used(std::exchange(other.used, 0)),
            alignment(std::exchange(
                other.alignment,
                alignof(std::max_align_t)
            ))
        {
        }

        Page& operator=(Page&& other) noexcept
        {
            if (this != &other)
            {
                release();

                base = std::exchange(other.base, nullptr);
                cap = std::exchange(other.cap, 0);
                used = std::exchange(other.used, 0);
                alignment = std::exchange(
                    other.alignment,
                    alignof(std::max_align_t)
                );
            }
            return *this;
        }

        ~Page() noexcept
        {
            release();
        }

        void release() noexcept
        {
            if (base != nullptr)
            {
                ::operator delete(base, std::align_val_t{ alignment });
            }

            base = nullptr;
            cap = 0;
            used = 0;
            alignment = alignof(std::max_align_t);
        }
    };

    using DestroyFunction = void (*)(void*, std::size_t) noexcept;

    struct DestructorEntry
    {
        void* object = nullptr;
        std::size_t count = 0;
        DestroyFunction destroy = nullptr;
    };

    std::size_t page_size_;
    std::size_t large_threshold_;

    static constexpr std::size_t max_page_size_ = 1u << 20;
    static constexpr std::byte poison_byte_{ 0xCD };

    std::vector<Page> pages_;
    std::vector<Page> large_;
    std::vector<DestructorEntry> destructors_;

public:
    /**
     * @brief Opaque snapshot of the arena's allocation and lifetime state.
     *
     * A checkpoint belongs to exactly one Arena and is valid only while its
     * recorded state remains reachable.
     */
    struct Checkpoint
    {
        const Arena* owner = nullptr;
        std::size_t page_count = 0;
        std::size_t page_used = 0;
        std::size_t large_count = 0;
        std::size_t destructor_count = 0;
    };

    explicit Arena(
        std::size_t page_size = 64 * 1024,
        std::size_t large_threshold = 16 * 1024
    );

    ~Arena() noexcept;

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;

    /**
     * @brief Allocates raw, uninitialized storage.
     * @param n Number of bytes to allocate; must be non-zero.
     * @param alignment Required power-of-two alignment.
     * @return Pointer to aligned storage or an allocation/argument error.
     *
     * Raw allocations do not register destructors. Use arena_new() or
     * arena_new_array() for arena-owned object lifetime.
     */
    [[nodiscard]] Result<void*> alloc(
        std::size_t n,
        std::size_t alignment = alignof(std::max_align_t)
    );

    /** @brief Captures a state that can later be passed to rollback(). */
    [[nodiscard]] Checkpoint checkpoint() const noexcept;

    /**
     * @brief Destroys objects and reclaims allocations made after @p cp.
     * @throws std::invalid_argument If the checkpoint is foreign or no longer
     *         reachable from the current arena state.
     */
    void rollback(const Checkpoint& cp);

    /**
     * @brief Destroys all arena-owned objects and resets allocation state.
     * @param release_all When false, retains the first normal page for reuse.
     */
    void reset(bool release_all = false) noexcept;

    [[nodiscard]] Result<std::uint64_t> get_used_bytes() const;
    [[nodiscard]] Result<std::uint64_t> get_reserved_bytes() const;

    [[nodiscard]] std::size_t get_pages_size() const noexcept;
    [[nodiscard]] std::size_t get_large_size() const noexcept;
    [[nodiscard]] std::size_t get_destructor_count() const noexcept;

    [[nodiscard]] Result<std::size_t> get_next_page_size(
        std::size_t n
    ) const;

private:
    template <class T, class... Args>
    friend Result<T*> arena_new(Arena& arena, Args&&... args);

    template <class T>
    friend Result<T*> arena_new_array(Arena& arena, std::size_t count);

    [[nodiscard]] static Result<std::size_t> align_up(
        std::size_t x,
        std::size_t alignment
    );

    [[nodiscard]] Result<void*> alloc_small(
        std::size_t n,
        std::size_t alignment
    );

    [[nodiscard]] Result<void*> alloc_large(
        std::size_t n,
        std::size_t alignment
    );

    [[nodiscard]] Result<bool> fits_in(
        const Page& page,
        std::size_t n,
        std::size_t alignment
    ) const;

    [[nodiscard]] Result<Page> alloc_page(
        std::size_t cap,
        std::size_t alignment
    );

    [[nodiscard]] bool register_destructor(
        void* object,
        std::size_t count,
        DestroyFunction destroy
    ) noexcept;

    void destroy_to(std::size_t destructor_count) noexcept;
    static void poison(void* memory, std::size_t size) noexcept;
};

namespace arena_detail
{
    template <class T>
    void destroy_objects(void* memory, std::size_t count) noexcept
    {
        static_assert(std::is_nothrow_destructible_v<T>);

        auto* objects = static_cast<T*>(memory);
        for (std::size_t i = count; i > 0; --i)
        {
            std::destroy_at(objects + (i - 1));
        }
    }
} // namespace arena_detail

template <class T, class... Args>
Result<T*> arena_new(Arena& arena, Args&&... args)
{
    static_assert(std::is_object_v<T>, "T must be an object type");
    static_assert(!std::is_array_v<T>, "Use arena_new_array for arrays");
    static_assert(
        std::is_nothrow_destructible_v<T>,
        "Arena-managed destructors must be noexcept"
        );

    const Arena::Checkpoint cp = arena.checkpoint();

    Result<void*> allocation = arena.alloc(sizeof(T), alignof(T));
    if (!allocation.is_ok())
    {
        return Result<T*>::fail(std::move(allocation.status));
    }

    auto* object_memory = static_cast<T*>(allocation.value);
    assert(object_memory != nullptr);

    try
    {
        T* object = std::construct_at(
            object_memory,
            std::forward<Args>(args)...
        );

        if constexpr (!std::is_trivially_destructible_v<T>)
        {
            if (!arena.register_destructor(
                object,
                1,
                &arena_detail::destroy_objects<T>
            ))
            {
                std::destroy_at(object);
                arena.rollback(cp);

                return Result<T*>::fail(
                    Status{
                        StatusCode::AllocationFailed,
                        "Failed to register arena object destructor"
                    }
                );
            }
        }

        return Result<T*>::ok(object);
    }
    catch (...)
    {
        arena.rollback(cp);
        throw;
    }
}

template <class T>
Result<T*> arena_new_array(Arena& arena, std::size_t count)
{
    static_assert(std::is_object_v<T>, "T must be an object type");
    static_assert(!std::is_array_v<T>, "Nested arrays are not supported");
    static_assert(
        std::is_nothrow_destructible_v<T>,
        "Arena-managed destructors must be noexcept"
        );

    if (count == 0)
    {
        return Result<T*>::fail(
            Status{ StatusCode::InvalidArgument, "Array count is zero" }
        );
    }

    if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
    {
        return Result<T*>::fail(
            Status{
                StatusCode::AllocationTooLarge,
                "Arena array byte size overflows size_t"
            }
        );
    }

    const Arena::Checkpoint cp = arena.checkpoint();

    Result<void*> allocation = arena.alloc(sizeof(T) * count, alignof(T));
    if (!allocation.is_ok())
    {
        return Result<T*>::fail(std::move(allocation.status));
    }

    auto* objects = static_cast<T*>(allocation.value);
    assert(objects != nullptr);

    try
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            T* object = std::construct_at(objects + i);

            if constexpr (!std::is_trivially_destructible_v<T>)
            {
                if (!arena.register_destructor(
                    object,
                    1,
                    &arena_detail::destroy_objects<T>
                ))
                {
                    std::destroy_at(object);
                    arena.rollback(cp);

                    return Result<T*>::fail(
                        Status{
                            StatusCode::AllocationFailed,
                            "Failed to register arena array destructor"
                        }
                    );
                }
            }
        }

        return Result<T*>::ok(objects);
    }
    catch (...)
    {
        arena.rollback(cp);
        throw;
    }
}

Result<ArenaEntry> arena_copy_bytes(
    Arena& arena,
    const void* src,
    std::uint32_t n
);
