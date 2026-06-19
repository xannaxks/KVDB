# Arena Page Size Allocation Strategies

An arena allocator does not allocate every object separately from the operating system or heap allocator. Instead, it allocates larger memory regions called **pages**, and then serves smaller allocations from those pages.

The page size allocation strategy defines how large those pages should be, how they should grow, and how unusually large allocations should be handled.

---

## 1. Fixed Page Size Strategy

In the fixed page size strategy, every arena page has the same size.

For example:

```cpp
page_size = 64KB;
```

Every time the current page runs out of space, the arena allocates another page of the same size.

Example page layout:

```text
Page 1: 64KB
Page 2: 64KB
Page 3: 64KB
Page 4: 64KB
```

### Advantages

* Simple to implement.
* Easy to reason about.
* Predictable memory usage.
* Good when most allocations are small and similar in size.

### Disadvantages

* May allocate too many pages for large workloads.
* Large individual allocations require special handling.
* Can waste memory if allocation sizes do not fit well into the fixed page size.

### Example

```cpp
if (current_page.remaining_space < allocation_size)
{
    allocate_new_page(64 * 1024);
}
```

This strategy is suitable for simple arenas where allocation patterns are predictable.

---

## 2. Geometric Page Growth Strategy

In the geometric growth strategy, each new page is larger than the previous one.

Usually, the next page is twice as large as the previous page.

Example:

```text
64KB -> 128KB -> 256KB -> 512KB -> 1MB -> ...
```

### Advantages

* Reduces the number of page allocations.
* Good for workloads where the arena grows over time.
* Improves performance by allocating larger chunks as memory demand increases.

### Disadvantages

* Can waste memory if the arena grows too aggressively.
* The final page may be much larger than necessary.
* Without a maximum limit, page sizes can become too large.

### Example

```cpp
new_page_size = previous_page_size * 2;
allocate_new_page(new_page_size);
```

A safer version usually applies a maximum page size:

```cpp
new_page_size = min(previous_page_size * 2, max_page_size);
```

Example capped growth:

```text
64KB -> 128KB -> 256KB -> 512KB -> 1MB -> 1MB -> 1MB
```

This strategy is useful for arenas that may grow significantly during execution.

---

## 3. Required-Size Page Strategy

In the required-size strategy, when an allocation does not fit into the current page, the arena allocates a new page that is just large enough for the requested allocation.

Example:

```cpp
if (current_page.remaining_space < allocation_size)
{
    allocate_new_page(allocation_size);
}
```

Example:

```text
Allocation needs 80KB
New page size = 80KB
```

### Advantages

* Avoids allocating more memory than needed for a specific allocation.
* Simple to implement.
* Useful for uncommon allocations that are larger than the normal page size.

### Disadvantages

* Can create many pages with irregular sizes.
* Poor locality if many differently-sized pages are created.
* Not ideal as the main arena growth strategy.

This strategy is usually better as a fallback, not as the primary strategy.

---

## 4. Geometric Growth With Required-Size Fallback

This strategy combines geometric growth with required-size allocation.

When a new page is needed, the arena chooses the larger value between:

1. the next geometrically grown page size,
2. the requested allocation size.

Example:

```cpp
new_page_size = max(previous_page_size * 2, allocation_size);
```

Example:

```text
Previous page size = 64KB
Requested allocation = 80KB

New page size = max(128KB, 80KB)
New page size = 128KB
```

Another example:

```text
Previous page size = 64KB
Requested allocation = 300KB

New page size = max(128KB, 300KB)
New page size = 300KB
```

### Advantages

* Handles both normal growth and larger allocations.
* Prevents allocating a page that is too small for the requested object.
* Reduces the number of page allocations over time.

### Disadvantages

* A single unusually large allocation can affect future page growth.
* If the oversized page becomes the new base size, future pages may become too large.
* Needs careful handling to avoid memory waste.

For example:

```text
64KB -> 128KB -> 300KB -> 600KB -> 1.2MB
```

The `300KB` page may have been needed only once, but now future pages grow from it.

A better design separates normal growth size from exceptional page size.

```cpp
normal_growth_size = 128KB;
actual_page_size = 300KB;
```

The arena can allocate a `300KB` page for the special allocation, but continue normal growth from `128KB`.

---

## 5. Large-Object Bypass Strategy

In this strategy, large allocations are not stored inside normal arena pages.

Instead, if an allocation is larger than some threshold, the arena creates a dedicated large allocation block for it.

Example:

```cpp
large_threshold = 16KB;
```

Then:

```cpp
if (allocation_size >= large_threshold)
{
    allocate_large_block(allocation_size);
}
else
{
    allocate_from_normal_page(allocation_size);
}
```

Example layout:

```text
Normal pages:
  Page 1: many small objects
  Page 2: many small objects

Large pages:
  Large block 1: one 32KB object
  Large block 2: one 80KB object
```

### Advantages

* Prevents large objects from wasting normal page space.
* Prevents large objects from distorting normal page growth.
* Good when most allocations are small but some values may be large.

### Disadvantages

* Requires separate tracking for large allocations.
* More complex than a simple arena.
* May increase the number of underlying memory allocations.

This strategy is very useful in database systems because keys and values can sometimes be unusually large.

---

## 6. Capped Geometric Growth Strategy

The capped geometric strategy grows pages geometrically, but only up to a maximum page size.

Example configuration:

```cpp
initial_page_size = 64KB;
max_page_size = 1MB;
growth_factor = 2;
```

Example page growth:

```text
64KB -> 128KB -> 256KB -> 512KB -> 1MB -> 1MB -> 1MB
```

### Advantages

* Avoids too many small page allocations.
* Prevents pages from growing without limit.
* Good balance between performance and memory usage.
* Suitable for long-running systems.

### Disadvantages

* Slightly more complex than simple geometric growth.
* Still needs separate handling for very large allocations.
* The maximum page size must be chosen carefully.

### Example

```cpp
new_page_size = previous_page_size * 2;

if (new_page_size > max_page_size)
{
    new_page_size = max_page_size;
}
```

This is usually a strong default strategy for a database arena allocator.

---

## 7. Size-Class Page Strategy

In the size-class strategy, pages are chosen from predefined size classes instead of growing freely.

Example size classes:

```text
64KB
256KB
1MB
4MB
```

If an allocation needs `100KB`, the arena may allocate a `256KB` page.

If an allocation needs `700KB`, the arena may allocate a `1MB` page.

### Advantages

* Predictable page sizes.
* Easier memory accounting.
* Avoids random page sizes.
* Similar to strategies used by more advanced allocators.

### Disadvantages

* More complex to implement.
* Can waste memory due to rounding up.
* Requires choosing good size classes.

Example:

```cpp
if (allocation_size <= 64KB)
    page_size = 64KB;
else if (allocation_size <= 256KB)
    page_size = 256KB;
else if (allocation_size <= 1MB)
    page_size = 1MB;
else
    allocate_large_block(allocation_size);
```

This strategy is more allocator-like and may be useful if the arena is expected to handle many different allocation sizes.

---

## 8. Recommended Strategy For This Arena

This arena uses a **capped geometric page growth strategy with large-object bypass**.

Normal allocations are served from normal arena pages. The first normal page starts with a fixed initial size. When the current page does not have enough space, a new page is allocated. The new page size grows geometrically, usually by doubling the previous normal page size, up to a fixed maximum page size.

Large allocations bypass normal pages and are stored in dedicated large blocks.

Example configuration:

```cpp
initial_page_size = 64KB;
max_page_size = 1MB;
large_threshold = 16KB;
growth_factor = 2;
```

Allocation logic:

```cpp
if (allocation_size >= large_threshold)
{
    allocate_large_block(allocation_size);
    return pointer;
}

if (current_page_has_enough_space(allocation_size))
{
    allocate_from_current_page(allocation_size);
    return pointer;
}

new_page_size = min(normal_growth_size * 2, max_page_size);
new_page_size = max(new_page_size, allocation_size);

allocate_normal_page(new_page_size);
allocate_from_current_page(allocation_size);
```

However, the arena should avoid letting one unusual allocation permanently affect future page growth.

For example, suppose the normal page size is `128KB`, but one allocation needs `300KB`.

A naive strategy may do this:

```text
128KB -> 300KB -> 600KB -> 1.2MB
```

This is risky because one unusual allocation changes the whole future growth curve.

A better strategy is:

```text
Normal growth size: 128KB
Special page size: 300KB
Next normal growth size: 256KB
```

In other words, exceptional pages should not always become the base for future normal page growth.

---

## Summary

| Strategy                           | Description                                         | Best Use Case                                     |
| ---------------------------------- | --------------------------------------------------- | ------------------------------------------------- |
| Fixed page size                    | Every page has the same size                        | Simple arenas with predictable allocation sizes   |
| Geometric growth                   | Each page grows, usually by doubling                | Arenas that grow over time                        |
| Required-size page                 | New page is exactly large enough for the allocation | Fallback for unusual allocations                  |
| Geometric + required-size fallback | New page is `max(grown_size, allocation_size)`      | General-purpose arena growth                      |
| Large-object bypass                | Large objects get dedicated blocks                  | Preventing large values from wasting normal pages |
| Capped geometric growth            | Geometric growth up to a maximum size               | Balanced production-style arena                   |
| Size-class pages                   | Pages are chosen from predefined size classes       | Advanced allocators with varied allocation sizes  |

---

## Final Design Statement

The arena allocator uses capped geometric page growth with large-object bypass.

Small and medium allocations are served from normal pages. Normal pages start from an initial size and grow by a fixed factor until reaching a maximum page size. Large allocations are stored separately in dedicated large blocks.

This design reduces the number of page allocations while preventing unusually large objects from wasting normal page space or distorting future page growth.
