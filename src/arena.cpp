#include "arena.h"
#include <bit>
#include <limits>
#include <numeric>
#include <span>
#include <format>

Result<ArenaEntry> ArenaEntry::make_entry(Arena& arena, const std::span<const std::byte> str)
{
	if (str.size() > std::numeric_limits<std::uint32_t>::max())
		return Result<ArenaEntry>::fail(
			Status{
				StatusCode::AllocationTooLarge,
				"Allocation size exceeds uint32_t limit"
			}
		);

	return arena_copy_bytes(
		arena,
		str.data(),
		static_cast<std::uint32_t>(str.size())
	);
}

Result<ArenaEntry> ArenaEntry::make_entry(Arena& arena, const std::string& str)
{
	if (str.size() > std::numeric_limits<std::uint32_t>::max())
		return Result<ArenaEntry>::fail(
			Status{
				StatusCode::AllocationTooLarge,
				"Allocation size exceeds uint32_t limit"
			}
		);

	return arena_copy_bytes(
		arena,
		reinterpret_cast<const std::byte*>(str.data()),
		static_cast<std::uint32_t>(str.size())
	);
}

std::string ArenaEntry::generate_random_key(const std::string& prefix, std::size_t length, bool fixed)
{
	assert(fixed || length > 0);

	int sz = fixed ? static_cast<int>(length) : rand() % length + 1;
	std::string result = prefix;
	while (sz--)
	{
		result += static_cast<char>(rand() % 256);
	}
	return result;
}

std::string ArenaEntry::generate_random_value(const std::string& prefix, std::size_t length, bool fixed)
{
	assert(fixed || length > 0);

	int sz = fixed ? static_cast<int>(length) : rand() % length + 1;
	std::string result = prefix;
	while (sz--)
	{
		result += static_cast<char>(rand() % 256);
	}
	return result;
}

Arena::Arena(std::size_t initial_page_size, std::size_t initial_large_threshold)
{
	if (initial_page_size == 0)
		throw std::invalid_argument("Arena initial page size can not be zero");
	if (initial_large_threshold == 0)
		throw std::invalid_argument("Arena initial large threshold can not be zero");
	if (initial_large_threshold < alignof(std::max_align_t))
		throw std::invalid_argument("Arena initial large threshold can not be less than alignof(std::max_align_t)");
	if (initial_page_size < alignof(std::max_align_t))
		throw std::invalid_argument("Arena initial page size can not be less than alignof(std::max_align_t)");

	this->page_size = initial_page_size;
	this->large_threshold = initial_large_threshold;
}
Arena::~Arena()
{
	Arena::reset(true);
}

Result<void*> Arena::alloc(size_t n, size_t align)
{
	if (n == 0)
		return Result<void*>::ok(nullptr);

	if (align == 0) {
		return Result<void*>::fail(
			Status{
				StatusCode::InvalidAlignment,
				"Alignment is zero"
			}
		);
	}

	if (!std::has_single_bit(align)) {
		return Result<void*>::fail(
			Status{
				StatusCode::InvalidAlignment,
				"Alignment is not a power of two"
			}
		);
	}

	if (align > alignof(std::max_align_t)) {
		return Result<void*>::fail(
			Status{
				StatusCode::InvalidAlignment,
				"Alignment is larger than alignof(std::max_align_t)"
			}
		);
	}

	if (n >= this->large_threshold)
		return this->alloc_large(n, align);
	return this->alloc_small(n, align);
}

Result<void*> Arena::alloc_small(size_t n, size_t align)
{
	if (pages.empty()) {
		auto size_result = get_next_page_size(n);
		if (!size_result.is_ok())
			return Result<void*>::fail(std::move(size_result.status));

		auto page_result = alloc_page(size_result.value);
		if (!page_result.is_ok())
			return Result<void*>::fail(std::move(page_result.status));

		pages.emplace_back(page_result.value);
	}

	auto fit_result = fits_in(pages.back(), n, align);
	if (!fit_result.is_ok())
		return Result<void*>::fail(std::move(fit_result.status));

	if (!fit_result.value) {
		auto size_result = get_next_page_size(n);
		if (!size_result.is_ok())
			return Result<void*>::fail(std::move(size_result.status));

		auto page_result = alloc_page(size_result.value);
		if (!page_result.is_ok())
			return Result<void*>::fail(std::move(page_result.status));

		pages.emplace_back(page_result.value);
	}

	Page& p = pages.back();

	auto off_result = align_up(p.used, align);
	if (!off_result.is_ok())
		return Result<void*>::fail(std::move(off_result.status));

	size_t off = off_result.value;

	if (off > p.cap || n > p.cap - off) {
		return Result<void*>::fail(
			Status{
				StatusCode::AllocationFailed,
				"Arena page does not have enough space"
			}
		);
	}

	void* out = reinterpret_cast<std::byte*>(p.base) + off;
	p.used = off + n;

	return Result<void*>::ok(out);
}

Result<void*> Arena::alloc_large(std::size_t n, std::size_t align)
{
	if (align > alignof(std::max_align_t))
		return Result<void*>::fail(
			Status{
				StatusCode::InvalidAlignment,
				"Alignment is larger than alignof(std::max_align_t)"
			}
		);

	Result<Page> p = alloc_page(n);
	if (!p.is_ok())
		return Result<void*>::fail(std::move(p.status));

	p.value.used = n;
	this->large.emplace_back(p.value);

	return Result<void*>::ok(p.value.base);
}

Result<Arena::Page> Arena::alloc_page(size_t cap) // consider catching std::bad_alloc on higher layers
{
	if (cap == 0) {
		return Result<Page>::fail(
			Status{
				StatusCode::InvalidArgument,
				"Arena page capacity is zero"
			}
		);
	}
	try {
		auto* ptr = static_cast<std::byte*>(
			::operator new(
				cap,
				std::align_val_t(alignof(std::max_align_t))
				)
			);
		std::memset(ptr, 0xCD, cap);

		return Result<Arena::Page>::ok(Page{ ptr, cap, 0 });
	}
	catch(const std::bad_alloc&)
	{
		return Result<Arena::Page>::fail(
			Status{
				StatusCode::AllocationFailed,
				std::format(
					"Failed to allocate arena page of size {} and alignment {}",
					cap,
					std::align_val_t(alignof(std::max_align_t))
				)
			}
		);
	}
}

// =============================================================================== 

Result<bool> Arena::fits_in(const Page& p, std::size_t n, std::size_t align) const
{
	auto off_result = Arena::align_up(p.used, align);
	if (!off_result.is_ok())
		return Result<bool>::fail(std::move(off_result.status));

	std::size_t off = off_result.value;

	if (off > p.cap)
		return Result<bool>::ok(false);

	return Result<bool>::ok(n <= p.cap - off);
}

Result<std::size_t> Arena::align_up(std::size_t x, std::size_t a)
{
    if (a == 0) {
        return Result<std::size_t>::fail(
            Status{StatusCode::InvalidAlignment, "Alignment is zero"}
        );
    }

    if (!std::has_single_bit(a)) {
        return Result<std::size_t>::fail(
            Status{StatusCode::InvalidAlignment, "Alignment is not a power of two"}
        );
    }

    const std::size_t mask = a - 1;

    if (x > std::numeric_limits<std::size_t>::max() - mask) {
        return Result<std::size_t>::fail(
            Status{
                StatusCode::AllocationTooLarge,
                "Arena align_up overflow"
            }
        );
    }

    return Result<std::size_t>::ok((x + mask) & ~mask);
}

Result<std::size_t> Arena::get_next_page_size(size_t n) const
{
	auto result_align_up = Arena::align_up(n, alignof(std::max_align_t));
	if (!result_align_up.is_ok())
		return Result<std::size_t>::fail(result_align_up.status);

	size_t required = result_align_up.value;

	if (required > this->max_page_size)
		return Result<std::size_t>::fail(
			Status{
				StatusCode::AllocationTooLarge,
				"Requested allocation size exceeds max_page_size"
			}
		);
	
	if (this->pages.empty())
		return Result<std::size_t>::ok(std::max(this->page_size, required));


	if (required > (this->max_page_size - 1) / 2)
		return Result<std::size_t>::ok(this->max_page_size);

	size_t prev = this->pages.back().cap;
	size_t doubled = (prev > this->max_page_size / 2) ? this->max_page_size : prev * 2;
	return Result<std::size_t>::ok(std::max(doubled, required));
}

// =============================================================================== 

Result<uint64_t> Arena::get_reserved_bytes() const
{
	uint64_t result = 0ull;
	for (auto& p : this->pages)
	{
		if (result > std::numeric_limits<uint64_t>::max() - p.cap)
			return Result<std::uint64_t>::fail(
				Status{
					StatusCode::AllocationTooLarge,
					"Amount of reserved bytes exceeds std::uint64_t::max"
				}
			);
		result += p.cap;
	}
	for (auto& l : this->large)
	{
		if (result > std::numeric_limits<uint64_t>::max() - l.cap)
			return Result<std::uint64_t>::fail(
				Status{
					StatusCode::AllocationTooLarge,
					"Amount of reserved bytes exceeds std::uint64_t::max"
				}
			);
		result += l.cap;
	}
	return Result<std::uint64_t>::ok(result);
}
Result<std::uint64_t> Arena::get_used_bytes() const
{
	uint64_t result = 0ull;
	for (auto& p : this->pages)
	{
		if (result > std::numeric_limits<uint64_t>::max() - p.used)
			return Result<std::uint64_t>::fail(
				Status{
					StatusCode::AllocationTooLarge,
					"Amount of used bytes exceeds std::uint64_t::max"
				}
			);
		result += p.used;
	}
	for (auto& l : this->large)
	{
		if (result > std::numeric_limits<uint64_t>::max() - l.used)
			return Result<std::uint64_t>::fail(
				Status{
					StatusCode::AllocationTooLarge,
					"Amount of used bytes exceeds std::uint64_t::max"
				}
			);
		result += l.used;
	}
	return Result<std::uint64_t>::ok(result);
}

// =============================================================================== 

Arena::Checkpoint Arena::checkpoint() const {
	return Arena::Checkpoint{
		this->pages.size(),
		this->pages.empty() ? 0 : this->pages.back().used,
		this->large.size()
	};
}
void Arena::rollback(const Arena::Checkpoint& cp)
{
	assert(cp.large_count <= this->large.size());
	assert(cp.page_count <= this->pages.size());
	if (cp.page_count > 0)
		assert(cp.page_used <= pages[cp.page_count - 1].cap);

	if (!this->pages.empty())
	{
		while (this->pages.size() > cp.page_count)
		{
			free_page(this->pages.back());
			this->pages.pop_back();
		}
		if (!this->pages.empty())
		{
			this->pages.back().used = std::min(this->pages.back().used, cp.page_used);
			memset(reinterpret_cast<std::byte*>(this->pages.back().base) + this->pages.back().used, 0xCD, this->pages.back().cap - this->pages.back().used);
		}
	}

	if (this->large.empty()) return;
	while (this->large.size() > cp.large_count)
	{
		free_page(this->large.back());
		this->large.pop_back();
	}
}
void Arena::reset(bool release_all)
{
	if (release_all)
	{
		for (auto& page : this->pages)
		{
			free_page(page);
		}
		this->pages.clear();
		for (auto& l : this->large)
		{
			free_page(l);
		}
		this->large.clear();
	}
	else
	{
		for (std::size_t i = 1; i < this->pages.size(); i++)
		{
			free_page(this->pages[i]);
		}
		if (!this->pages.empty())
		{
			this->pages[0].used = 0;
			memset(this->pages[0].base, 0xCD, this->pages[0].cap);
		}
		this->pages.resize(std::min<std::size_t>(this->pages.size(), 1));
		for (auto& l : this->large)
		{
			free_page(l);
		}
		this->large.clear();
	}
}
void Arena::free_page(Page& p) noexcept
{
	assert(p.base != nullptr || p.cap == 0);
	assert(p.used <= p.cap);

	::operator delete(p.base, std::align_val_t(alignof(std::max_align_t)));

	p.base = nullptr;
	p.cap = p.used = 0;
}

// =============================================================================== 

Result<ArenaEntry> arena_copy_bytes(Arena& a, const void* src, std::uint32_t n)
{
	if (n == 0)
		return Result<ArenaEntry>::ok(
			ArenaEntry{ nullptr, 0 }
		);

	if (src == nullptr)
		return Result<ArenaEntry>::fail(
			Status{
				StatusCode::NullPointer,
				"Source pointer is null"
			}
		);

	auto result = a.alloc(n, alignof(std::byte));
	if (!result.is_ok())
		return Result<ArenaEntry>::fail(std::move(result.status));

	void* dst = result.value;
	std::memcpy(dst, src, n);

	return Result<ArenaEntry>::ok(
		ArenaEntry{ dst, n }
	);
}

// =============================================================================== 

std::size_t Arena::get_pages_size() const
{
	return this->pages.size();
}
std::size_t Arena::get_large_size() const
{
	return this->large.size();
}

// ================================================================================ 
ArenaEntry::ArenaEntry(void* ptr, std::size_t entry_size)
	: data(ptr)
{
	if (entry_size > std::numeric_limits<std::uint32_t>::max())
		throw std::invalid_argument("Entry size excceds std::uint32_t::max");

	size = static_cast<std::uint32_t>(entry_size);

	if (size > 0 && data == nullptr)
		throw std::invalid_argument("Provided size can not correspond to nullptr");
}

ArenaEntry::ArenaEntry(ArenaEntry&& other) noexcept
	: data(other.data), size(other.size)
{
	other.data = nullptr;
	other.size = 0;
}

ArenaEntry& ArenaEntry::operator=(ArenaEntry&& other) noexcept
{
	if (this == &other)
		return *this;

	data = other.data;
	size = other.size;

	other.data = nullptr;
	other.size = 0;

	return *this;
}

bool ArenaEntry::operator<(const ArenaEntry& other) const
{
	assert(data != nullptr || size == 0);
	assert(other.data != nullptr || other.size == 0);

	const std::size_t common = std::min<std::size_t>(size, other.size);

	if (common > 0)
	{
		assert(data != nullptr && other.data != nullptr);

		const int result = std::memcmp(data, other.data, common);

		if (result < 0) return true;
		if (result > 0) return false;
	}

	return size < other.size;
}

bool ArenaEntry::operator>(const ArenaEntry& other) const
{
	return other < *this;
}

bool ArenaEntry::operator==(const ArenaEntry& other) const
{
	assert(data != nullptr || size == 0);
	assert(other.data != nullptr || other.size == 0);

	if (size != other.size)
		return false;

	if (size == 0)
		return true;

	return std::memcmp(data, other.data, size) == 0;
}

bool ArenaEntry::operator>=(const ArenaEntry& other) const
{
	return (*this > other) || (*this == other);
}

bool ArenaEntry::operator<=(const ArenaEntry& other) const
{
	return (*this < other) || (*this == other);
}
