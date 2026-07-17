#include "arena.h"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

Result<ArenaEntry> ArenaEntry::make_entry(
    Arena& arena,
    std::span<const std::byte> bytes
)
{
    if (bytes.size() > std::numeric_limits<std::uint32_t>::max())
    {
        return Result<ArenaEntry>::fail(
            Status{
                StatusCode::AllocationTooLarge,
                "Allocation size exceeds uint32_t limit"
            }
        );
    }

    return arena_copy_bytes(
        arena,
        bytes.data(),
        static_cast<std::uint32_t>(bytes.size())
    );
}

Result<ArenaEntry> ArenaEntry::make_entry(
    Arena& arena,
    const std::string& str
)
{
    if (str.size() > std::numeric_limits<std::uint32_t>::max())
    {
        return Result<ArenaEntry>::fail(
            Status{
                StatusCode::AllocationTooLarge,
                "Allocation size exceeds uint32_t limit"
            }
        );
    }

    return arena_copy_bytes(
        arena,
        str.data(),
        static_cast<std::uint32_t>(str.size())
    );
}

std::string ArenaEntry::generate_random_key(
    const std::string& prefix,
    std::size_t length,
    bool fixed
)
{
    assert(fixed || length > 0);

    const std::size_t generated_length = fixed
        ? length
        : static_cast<std::size_t>(std::rand()) % length + 1;

    std::string result = prefix;
    result.reserve(prefix.size() + generated_length);

    for (std::size_t i = 0; i < generated_length; ++i)
    {
        result.push_back(static_cast<char>(std::rand() % 256));
    }

    return result;
}

std::string ArenaEntry::generate_random_value(
    const std::string& prefix,
    std::size_t length,
    bool fixed
)
{
    return generate_random_key(prefix, length, fixed);
}

Arena::Arena(
    std::size_t initial_page_size,
    std::size_t initial_large_threshold
)
    : page_size_(initial_page_size),
    large_threshold_(initial_large_threshold)
{
    if (initial_page_size == 0)
    {
        throw std::invalid_argument("Arena page size cannot be zero");
    }

    if (initial_page_size > max_page_size_)
    {
        throw std::invalid_argument(
            "Arena page size exceeds the maximum normal page size"
        );
    }

    if (initial_large_threshold == 0)
    {
        throw std::invalid_argument("Arena large threshold cannot be zero");
    }
}

Arena::~Arena() noexcept
{
    reset(true);
}

Result<void*> Arena::alloc(std::size_t n, std::size_t alignment) // call only when u want to manage object lifetime manually
{
    if (n == 0)
    {
        return Result<void*>::ok(nullptr);
    }

    if (alignment == 0)
    {
        return Result<void*>::fail(
            Status{ StatusCode::InvalidAlignment, "Alignment is zero" }
        );
    }

    if (!std::has_single_bit(alignment))
    {
        return Result<void*>::fail(
            Status{
                StatusCode::InvalidAlignment,
                "Alignment is not a power of two"
            }
        );
    }

    // Normal pages are max_align_t-aligned. Over-aligned objects receive a
    // dedicated allocation so their alignment is preserved.
    if (n >= large_threshold_ ||
        n > max_page_size_ ||
        alignment > alignof(std::max_align_t))
    {
        return alloc_large(n, alignment);
    }

    return alloc_small(n, alignment);
}

Result<void*> Arena::alloc_small(
    std::size_t n,
    std::size_t alignment
)
{
    auto append_page = [this, n]() -> Result<bool>
        {
            Result<std::size_t> size_result = get_next_page_size(n);
            if (!size_result.is_ok())
            {
                return Result<bool>::fail(std::move(size_result.status));
            }

            Result<Page> page_result = alloc_page(
                size_result.value,
                alignof(std::max_align_t)
            );
            if (!page_result.is_ok())
            {
                return Result<bool>::fail(std::move(page_result.status));
            }

            try
            {
                pages_.emplace_back(std::move(page_result.value));
            }
            catch (const std::bad_alloc&)
            {
                return Result<bool>::fail(
                    Status{
                        StatusCode::AllocationFailed,
                        "Failed to grow Arena normal-page metadata"
                    }
                );
            }

            return Result<bool>::ok(true);
        };

    if (pages_.empty())
    {
        Result<bool> append_result = append_page();
        if (!append_result.is_ok())
        {
            return Result<void*>::fail(std::move(append_result.status));
        }
    }

    Result<bool> fit_result = fits_in(pages_.back(), n, alignment);
    if (!fit_result.is_ok())
    {
        return Result<void*>::fail(std::move(fit_result.status));
    }

    if (!fit_result.value)
    {
        Result<bool> append_result = append_page();
        if (!append_result.is_ok())
        {
            return Result<void*>::fail(std::move(append_result.status));
        }
    }

    Page& page = pages_.back();

    Result<std::size_t> offset_result = align_up(page.used, alignment);
    if (!offset_result.is_ok())
    {
        return Result<void*>::fail(std::move(offset_result.status));
    }

    const std::size_t offset = offset_result.value;
    if (offset > page.cap || n > page.cap - offset)
    {
        return Result<void*>::fail(
            Status{
                StatusCode::AllocationFailed,
                "Arena page does not have enough space"
            }
        );
    }

    void* output = static_cast<std::byte*>(page.base) + offset;
    page.used = offset + n;

    return Result<void*>::ok(output);
}

Result<void*> Arena::alloc_large(
    std::size_t n,
    std::size_t alignment
)
{
    Result<Page> page_result = alloc_page(n, alignment);
    if (!page_result.is_ok())
    {
        return Result<void*>::fail(std::move(page_result.status));
    }

    page_result.value.used = n;

    try
    {
        large_.emplace_back(std::move(page_result.value));
    }
    catch (const std::bad_alloc&)
    {
        return Result<void*>::fail(
            Status{
                StatusCode::AllocationFailed,
                "Failed to grow Arena large-allocation metadata"
            }
        );
    }

    return Result<void*>::ok(large_.back().base);
}

Result<Arena::Page> Arena::alloc_page(
    std::size_t cap,
    std::size_t alignment
)
{
    if (cap == 0)
    {
        return Result<Page>::fail(
            Status{ StatusCode::InvalidArgument, "Arena page capacity is zero" }
        );
    }

    try
    {
        void* memory = ::operator new(cap, std::align_val_t{ alignment });
        poison(memory, cap);

        return Result<Page>::ok(Page{ memory, cap, 0, alignment });
    }
    catch (const std::bad_alloc&)
    {
        return Result<Page>::fail(
            Status{
                StatusCode::AllocationFailed,
                "Failed to allocate an arena page"
            }
        );
    }
}

Result<bool> Arena::fits_in(
    const Page& page,
    std::size_t n,
    std::size_t alignment
) const
{
    Result<std::size_t> offset_result = align_up(page.used, alignment);
    if (!offset_result.is_ok())
    {
        return Result<bool>::fail(std::move(offset_result.status));
    }

    const std::size_t offset = offset_result.value;
    if (offset > page.cap)
    {
        return Result<bool>::ok(false);
    }

    return Result<bool>::ok(n <= page.cap - offset);
}

Result<std::size_t> Arena::align_up(
    std::size_t x,
    std::size_t alignment
)
{
    if (alignment == 0)
    {
        return Result<std::size_t>::fail(
            Status{ StatusCode::InvalidAlignment, "Alignment is zero" }
        );
    }

    if (!std::has_single_bit(alignment))
    {
        return Result<std::size_t>::fail(
            Status{
                StatusCode::InvalidAlignment,
                "Alignment is not a power of two"
            }
        );
    }

    const std::size_t mask = alignment - 1;
    if (x > std::numeric_limits<std::size_t>::max() - mask)
    {
        return Result<std::size_t>::fail(
            Status{
                StatusCode::AllocationTooLarge,
                "Arena alignment calculation overflowed"
            }
        );
    }

    return Result<std::size_t>::ok((x + mask) & ~mask);
}

Result<std::size_t> Arena::get_next_page_size(std::size_t n) const
{
    if (n == 0)
    {
        return Result<std::size_t>::fail(
            Status{ StatusCode::InvalidArgument, "Requested page size is zero" }
        );
    }

    if (n > max_page_size_)
    {
        return Result<std::size_t>::fail(
            Status{
                StatusCode::AllocationTooLarge,
                "Requested allocation exceeds maximum normal page size"
            }
        );
    }

    if (pages_.empty())
    {
        return Result<std::size_t>::ok(std::max(page_size_, n));
    }

    const std::size_t previous = pages_.back().cap;
    const std::size_t doubled = previous > max_page_size_ / 2
        ? max_page_size_
        : previous * 2;

    return Result<std::size_t>::ok(std::max(doubled, n));
}

Result<std::uint64_t> Arena::get_reserved_bytes() const
{
    std::uint64_t total = 0;

    const auto add_capacity = [&total](const Page& page) -> bool
        {
            if (total > std::numeric_limits<std::uint64_t>::max() - page.cap)
            {
                return false;
            }

            total += static_cast<std::uint64_t>(page.cap);
            return true;
        };

    for (const Page& page : pages_)
    {
        if (!add_capacity(page))
        {
            return Result<std::uint64_t>::fail(
                Status{
                    StatusCode::AllocationTooLarge,
                    "Arena reserved-byte count overflowed uint64_t"
                }
            );
        }
    }

    for (const Page& page : large_)
    {
        if (!add_capacity(page))
        {
            return Result<std::uint64_t>::fail(
                Status{
                    StatusCode::AllocationTooLarge,
                    "Arena reserved-byte count overflowed uint64_t"
                }
            );
        }
    }

    return Result<std::uint64_t>::ok(total);
}

Result<std::uint64_t> Arena::get_used_bytes() const
{
    std::uint64_t total = 0;

    const auto add_used = [&total](const Page& page) -> bool
        {
            if (total > std::numeric_limits<std::uint64_t>::max() - page.used)
            {
                return false;
            }

            total += static_cast<std::uint64_t>(page.used);
            return true;
        };

    for (const Page& page : pages_)
    {
        if (!add_used(page))
        {
            return Result<std::uint64_t>::fail(
                Status{
                    StatusCode::AllocationTooLarge,
                    "Arena used-byte count overflowed uint64_t"
                }
            );
        }
    }

    for (const Page& page : large_)
    {
        if (!add_used(page))
        {
            return Result<std::uint64_t>::fail(
                Status{
                    StatusCode::AllocationTooLarge,
                    "Arena used-byte count overflowed uint64_t"
                }
            );
        }
    }

    return Result<std::uint64_t>::ok(total);
}

Arena::Checkpoint Arena::checkpoint() const noexcept
{
    return Checkpoint{
        this,
        pages_.size(),
        pages_.empty() ? 0 : pages_.back().used,
        large_.size(),
        destructors_.size()
    };
}

void Arena::rollback(const Checkpoint& cp)
{
    const bool valid =
        cp.owner == this &&
        cp.page_count <= pages_.size() &&
        cp.large_count <= large_.size() &&
        cp.destructor_count <= destructors_.size() &&
        (cp.page_count == 0 ||
            cp.page_used <= pages_[cp.page_count - 1].used);

    if (!valid)
    {
        throw std::invalid_argument(
            "Arena checkpoint is foreign or no longer reachable"
        );
    }

    // Objects must die while their backing pages are still allocated.
    destroy_to(cp.destructor_count);

    if (pages_.size() > cp.page_count)
    {
        pages_.resize(cp.page_count);
    }

    if (cp.page_count > 0)
    {
        Page& checkpoint_page = pages_.back();
        if (checkpoint_page.used > cp.page_used)
        {
            poison(
                static_cast<std::byte*>(checkpoint_page.base) + cp.page_used,
                checkpoint_page.used - cp.page_used
            );
            checkpoint_page.used = cp.page_used;
        }
    }

    if (large_.size() > cp.large_count)
    {
        large_.resize(cp.large_count);
    }
}

void Arena::reset(bool release_all) noexcept
{
    destroy_to(0);

    large_.clear();

    if (release_all)
    {
        pages_.clear();
        return;
    }

    if (!pages_.empty())
    {
        Page& first = pages_.front();
        poison(first.base, first.cap);
        first.used = 0;

        if (pages_.size() > 1)
        {
            pages_.resize(1);
        }
    }
}

bool Arena::register_destructor(
    void* object,
    std::size_t count,
    DestroyFunction destroy
) noexcept
{
    assert(object != nullptr);
    assert(count > 0);
    assert(destroy != nullptr);

    try
    {
        destructors_.push_back(DestructorEntry{ object, count, destroy });
        return true;
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
}

void Arena::destroy_to(std::size_t destructor_count) noexcept
{
    assert(destructor_count <= destructors_.size());

    while (destructors_.size() > destructor_count)
    {
        const DestructorEntry entry = destructors_.back();
        destructors_.pop_back();
        entry.destroy(entry.object, entry.count);
    }
}

void Arena::poison(void* memory, std::size_t size) noexcept
{
    if (memory == nullptr || size == 0)
    {
        return;
    }

    std::memset(memory, std::to_integer<int>(poison_byte_), size);
}

Result<ArenaEntry> arena_copy_bytes(
    Arena& arena,
    const void* src,
    std::uint32_t n
)
{
    if (n == 0)
    {
        return Result<ArenaEntry>::ok(ArenaEntry{});
    }

    if (src == nullptr)
    {
        return Result<ArenaEntry>::fail(
            Status{ StatusCode::NullPointer, "Source pointer is null" }
        );
    }

    Result<void*> allocation = arena.alloc(n, alignof(std::byte));
    if (!allocation.is_ok())
    {
        return Result<ArenaEntry>::fail(std::move(allocation.status));
    }

    std::memcpy(allocation.value, src, n);
    return Result<ArenaEntry>::ok(ArenaEntry{ allocation.value, n });
}

std::size_t Arena::get_pages_size() const noexcept
{
    return pages_.size();
}

std::size_t Arena::get_large_size() const noexcept
{
    return large_.size();
}

std::size_t Arena::get_destructor_count() const noexcept
{
    return destructors_.size();
}

ArenaEntry::ArenaEntry(void* ptr, std::size_t entry_size)
    : data(ptr)
{
    if (entry_size > std::numeric_limits<std::uint32_t>::max())
    {
        throw std::invalid_argument("Entry size exceeds uint32_t::max");
    }

    size = static_cast<std::uint32_t>(entry_size);

    if (size > 0 && data == nullptr)
    {
        throw std::invalid_argument(
            "A non-empty ArenaEntry cannot have a null data pointer"
        );
    }
}

ArenaEntry::ArenaEntry(ArenaEntry&& other) noexcept
    : data(std::exchange(other.data, nullptr)),
    size(std::exchange(other.size, 0))
{
}

ArenaEntry& ArenaEntry::operator=(ArenaEntry&& other) noexcept
{
    if (this != &other)
    {
        data = std::exchange(other.data, nullptr);
        size = std::exchange(other.size, 0);
    }

    return *this;
}

bool ArenaEntry::operator<(const ArenaEntry& other) const noexcept
{
    assert(data != nullptr || size == 0);
    assert(other.data != nullptr || other.size == 0);

    const std::size_t common = std::min<std::size_t>(size, other.size);
    if (common > 0)
    {

        const int comparison = std::memcmp(data, other.data, common);
        if (comparison != 0)
        {
            return comparison < 0;
        }
    }

    return size < other.size;
}

bool ArenaEntry::operator>(const ArenaEntry& other) const noexcept
{
    return other < *this;
}

bool ArenaEntry::operator==(const ArenaEntry& other) const noexcept
{
    assert(data != nullptr || size == 0);
    assert(other.data != nullptr || other.size == 0);

    if (size != other.size)
    {
        return false;
    }

    return size == 0 || std::memcmp(data, other.data, size) == 0;
}

bool ArenaEntry::operator>=(const ArenaEntry& other) const noexcept
{
    return !(*this < other);
}

bool ArenaEntry::operator<=(const ArenaEntry& other) const noexcept
{
    return !(other < *this);
}