#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>
#include <utility>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <limits>
#include <cassert>

struct ByteSpan
{
	const std::byte* data = nullptr;
	std::uint32_t size = 0;
};

struct ArenaEntry
{
	void* data;
	std::size_t size;

	bool operator<(const ArenaEntry& other) const;
	bool operator>(const ArenaEntry& other) const;
	bool operator==(const ArenaEntry& other) const;

	ArenaEntry(void* ptr, std::size_t size);
	ArenaEntry() = default;

};

class Arena
{
private: 
	struct Page
	{
		std::byte* base = nullptr;
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
	void* alloc(std::size_t n, std::size_t align = alignof(std::max_align_t));

	// Save the current allocation state for later rollbacks.
	[[nodiscard]] Checkpoint checkpoint() const;

	// Rolls back arena back to a previously saved checkpoint.
	void rollback(const Checkpoint& cp);
	// Resets the arena.
	// If release_all == true, frees all the allocated space
	// Otherwise keeps one small block for reuse.
	void reset(bool release_all = false);

	uint64_t get_used_bytes() const;
	uint64_t get_reserved_bytes() const;

	std::size_t get_pages_size() const;
	std::size_t get_large_size() const;

	size_t get_next_page_size(size_t n) const;

private:

	static std::size_t align_up(std::size_t x, std::size_t a);

	void* alloc_small(std::size_t n, std::size_t align = alignof(std::max_align_t));
	void* alloc_large(std::size_t n, std::size_t align = alignof(std::max_align_t));

	[[nodiscard]] bool fits_in(const Page& p, std::size_t n, std::size_t align) const;

	[[nodiscard]] Page alloc_page(std::size_t cap);
	void free_page(Page& p);
};

template<typename T, class... Args>
inline T* arena_new(Arena& a, Args&&... args)
{
	void* mem = a.alloc(sizeof(T), alignof(T));
	return new (mem) T(std::forward<Args>(args)...);
}
ByteSpan arena_copy_bytes(Arena& a, const void* src, std::uint32_t n);