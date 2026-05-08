#include "arena.h"

ArenaEntry ArenaEntry::make_entry(Arena& arena, const std::string& str)
{
	if (str.size() > std::numeric_limits<std::uint32_t>::max())
		std::terminate();

	return arena_copy_bytes(
		arena,
		str.data(),
		static_cast<std::uint32_t>(str.size())
	);
}

std::string ArenaEntry::generate_random_key(const std::string& prefix, std::size_t length, bool fixed)
{
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
	if (initial_page_size == 0
		|| initial_large_threshold == 0
		|| initial_large_threshold < alignof(std::max_align_t)
		|| initial_page_size < alignof(std::max_align_t)
		)
		std::terminate();
	this->page_size = initial_page_size;
	this->large_threshold = initial_large_threshold;
}
Arena::~Arena()
{
	Arena::reset(true);
}

void* Arena::alloc(size_t n, size_t align)
{
	if (n == 0) return nullptr;
	if (align & (align - 1) || align == 0 || align > alignof(std::max_align_t)) std::terminate();

	if (n >= this->large_threshold)
		return this->alloc_large(n, align);
	return this->alloc_small(n, align);
}
void* Arena::alloc_small(size_t n, size_t align)
{
	if (pages.empty() || !fits_in(pages.back(), n, align))
	{
		this->pages.push_back(
			alloc_page(
				Arena::get_next_page_size(n)
			)
		);
	}

	Page& p = this->pages.back();
	size_t off = Arena::align_up(p.used, align);

	void* out = reinterpret_cast<std::byte*>(p.base) + off;
	if (off > std::numeric_limits<std::size_t>::max() - n)
		std::terminate();
	p.used = off + n;

	return out;
}
void* Arena::alloc_large(std::size_t n, std::size_t align)
{
	if (align > alignof(std::max_align_t))
		std::terminate();

	Page p = alloc_page(n);
	p.used = n;
	this->large.push_back(p);

	return p.base;
}
Arena::Page Arena::alloc_page(size_t cap)
{
	std::byte* ptr = static_cast<std::byte*>(
		::operator new(
			cap,
			std::align_val_t(alignof(std::max_align_t))
			)
		);
	memset(ptr, 0xCD, cap);
	return Page{ ptr, cap, 0 };
}

// =============================================================================== 

bool Arena::fits_in(const Page& p, std::size_t n, std::size_t align) const
{
	std::size_t require = Arena::align_up(p.used, align);
	if (require > std::numeric_limits<std::size_t>::max() - n)
		std::terminate();
	return require + n <= p.cap;
}
std::size_t Arena::align_up(std::size_t x, std::size_t a) // x refers to the offset, a is the alignment
{
	if (x > std::numeric_limits<std::size_t>::max() - a + 1)
		std::terminate();

	return (x + a - 1) & ~(a - 1);
}
size_t Arena::get_next_page_size(size_t n) const
{
	size_t required = Arena::align_up(n, alignof(std::max_align_t));

	if (this->pages.empty())
		return std::max(this->page_size, required);

	if (required > this->max_page_size)
		std::terminate();

	if (required > (this->max_page_size - 1) / 2)
		return this->max_page_size;

	size_t prev = this->pages.back().cap;
	size_t doubled = (prev > this->max_page_size / 2) ? this->max_page_size : prev * 2;
	return std::max(doubled, required);
}

// =============================================================================== 

uint64_t Arena::get_reserved_bytes() const
{
	uint64_t result = 0ull;
	for (auto& p : this->pages)
	{
		if (result > std::numeric_limits<uint64_t>::max() - p.cap)
			std::terminate();
		result += p.cap;
	}
	for (auto& l : this->large)
	{
		if (result > std::numeric_limits<uint64_t>::max() - l.cap)
			std::terminate();
		result += l.cap;
	}
	return result;
}
uint64_t Arena::get_used_bytes() const
{
	uint64_t result = 0ull;
	for (auto& p : this->pages)
	{
		if (result > std::numeric_limits<uint64_t>::max() - p.used)
			std::terminate();
		result += p.used;
	}
	for (auto& l : this->large)
	{
		if (result > std::numeric_limits<uint64_t>::max() - l.used)
			std::terminate();
		result += l.used;
	}
	return result;
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
void Arena::free_page(Page& p)
{
	::operator delete(p.base, std::align_val_t(alignof(std::max_align_t)));
	p.base = nullptr;
	p.cap = p.used = 0;
}

// =============================================================================== 

ArenaEntry arena_copy_bytes(Arena& a, const void* src, std::uint32_t n)
{
	if (n == 0)
		return ArenaEntry{ nullptr, 0 };

	if (src == nullptr)
		std::terminate();

	void* dst = a.alloc(n, alignof(std::byte));
	std::memcpy(dst, src, n);

	return ArenaEntry{ dst, n };
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
		std::terminate();

	size = static_cast<std::uint32_t>(entry_size);

	if (size > 0 && data == nullptr)
		std::terminate();
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
