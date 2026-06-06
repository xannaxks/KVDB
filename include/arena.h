#pragma once
#define NOMINMAX

#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>
#include "status.h"
#include <utility>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <span>
#include <limits>
#include <cassert>

class Arena;

struct ArenaEntry
{
	void* data = nullptr;
	std::uint32_t size = 0;

	ArenaEntry() = default;

	ArenaEntry(void* ptr, std::size_t entry_size);

	ArenaEntry(const ArenaEntry&) = default;
	ArenaEntry& operator=(const ArenaEntry&) = default;

	ArenaEntry(ArenaEntry&& other) noexcept;
	ArenaEntry& operator=(ArenaEntry&& other) noexcept;

	static std::string generate_random_key(const std::string& prefix, const std::size_t length = 100, bool fixed = true);
	static std::string generate_random_value(const std::string& prefix, const std::size_t length = 1000, bool fixed = true);

	bool operator<(const ArenaEntry& other) const;
	bool operator>(const ArenaEntry& other) const;
	bool operator==(const ArenaEntry& other) const;

	static Result<ArenaEntry> make_entry(Arena& arena, const std::span<const std::byte> str);
	static Result<ArenaEntry> make_entry(Arena& arena, const std::string& str);
};


class Arena
{
private: 
	struct Page
	{
		void* base = nullptr;
		std::size_t cap = 0;
		std::size_t used = 0;
	};

	std::size_t page_size;
	std::size_t large_threshold;
	const std::size_t max_page_size = 1u << 20;

	std::vector<Page> pages;
	std::vector<Page> large;

public:
	struct Checkpoint
	{
		std::size_t page_count;
		std::size_t page_used;
		std::size_t large_count;
	};

public:
	explicit Arena(std::size_t page_size = 64 * 1024, std::size_t large_threshold = 16 * 1024);

	~Arena();

	Arena(const Arena&) = delete;
	Arena& operator=(const Arena&) = delete;
	Arena(Arena&&) = delete;
	Arena& operator=(Arena&&) = delete;

	// Allocates raw memory with requested size and alignment.
	// Precondition: align must be power of two.
	Result<void*> alloc(std::size_t n, std::size_t align = alignof(std::max_align_t));

	// Save the current allocation state for later rollbacks.
	[[nodiscard]] Checkpoint checkpoint() const;

	// Rolls back arena back to a previously saved checkpoint.
	void rollback(const Checkpoint& cp);
	// Resets the arena.
	// If release_all == true, frees all the allocated space
	// Otherwise keeps one small block for reuse.
	void reset(bool release_all = false);

	Result<uint64_t> get_used_bytes() const;
	Result<uint64_t> get_reserved_bytes() const;

	std::size_t get_pages_size() const;
	std::size_t get_large_size() const;

	Result<size_t> get_next_page_size(size_t n) const;

private:

	static Result<std::size_t> align_up(std::size_t x, std::size_t a);

	Result<void*> alloc_small(std::size_t n, std::size_t align = alignof(std::max_align_t));
	Result<void*> alloc_large(std::size_t n, std::size_t align = alignof(std::max_align_t));

	Result<bool> fits_in(const Page& p, std::size_t n, std::size_t align) const;

	Result<Page> alloc_page(std::size_t cap);
	void free_page(Page& p) noexcept;
};

template<typename T, class... Args>
inline Result<T*> arena_new(Arena& a, Args&&... args)
{
	Result<void*> alloc_result = a.alloc(sizeof(T), alignof(T));
	if (!alloc_result.is_ok())
		return Result<T*>::fail(std::move(alloc_result.status));

	void* mem = alloc_result.value;
	assert(mem != nullptr);

	return Result<T*>::ok(new (mem) T(std::forward<Args>(args)...));
}

template <class T>
Result<T*> arena_new_array(Arena& arena, std::size_t count)
{
	if (count == 0)
		return Result<T*>::fail(Status{ StatusCode::InvalidArgument, "Count is zero" });

	Result<void*> alloc_result = arena.alloc(sizeof(T) * count, alignof(T));
	if(!alloc_result.is_ok())
		return Result<T*>::fail(std::move(alloc_result.status));

	void* mem = alloc_result.value;
	assert(mem != nullptr);

	T* ptr = static_cast<T*>(mem);

	for (std::size_t i = 0; i < count; ++i)
		new (&ptr[i]) T();

	return Result<T*>::ok(ptr);
}

Result<ArenaEntry> arena_copy_bytes(Arena& a, const void* src, std::uint32_t n);
