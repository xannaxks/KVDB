#include "file_helpers.h"

#include <cassert>
#include <cstdint>
#include <cstddef>
#include <fstream>
#include <vector>


bool fits_in_block(std::uint64_t offset,
    std::size_t size,
    std::uint32_t block_size)
{
    assert(block_size > 0);

    if (size == 0)
        return true;

    const std::uint64_t in_block = offset % block_size;

    return size <= block_size - in_block;
}


// =========================
// Write helpers
// =========================

bool align_to_block_boundary(WritableFile& file,
    std::uint64_t& offset,
    std::uint32_t block_size)
{
    assert(block_size > 0);
    assert(offset == file.current_position());

    const std::uint64_t in_block = offset % block_size;

    if (in_block == 0)
        return true;

    const std::uint64_t pad = block_size - in_block;

    std::vector<char> poison(static_cast<std::size_t>(pad),
        static_cast<char>(0xCD));

    return file.append(poison.data(), poison.size(), offset);
}


bool ensure_fits_in_block(WritableFile& file,
    std::size_t size,
    std::uint64_t& offset,
    std::uint32_t block_size)
{
    assert(block_size > 0);
    assert(offset == file.current_position());

    if (fits_in_block(offset, size, block_size))
        return true;

    return align_to_block_boundary(file, offset, block_size);
}


// =========================
// Read helpers
// =========================

bool align_to_block_boundary(ReadableFile& file,
    std::uint64_t& offset,
    std::uint32_t block_size)
{
    assert(block_size > 0);

    const std::uint64_t in_block = offset % block_size;

    if (in_block == 0)
        return true;

    const std::uint64_t new_offset = offset + (block_size - in_block);
    offset = new_offset;

    return true;
}


bool ensure_fits_in_block(ReadableFile& file,
    std::size_t size,
    std::uint64_t& offset,
    std::uint32_t block_size)
{
    assert(block_size > 0);

    if (fits_in_block(offset, size, block_size))
        return true;

    return align_to_block_boundary(file, offset, block_size);
}


bool move_g_to_next_block(ReadableFile& file,
    std::uint64_t& offset,
    std::uint32_t block_size)
{
    assert(block_size > 0);

    const std::uint64_t in_block = offset % block_size;

    const std::uint64_t jump =
        (in_block == 0) ? block_size : (block_size - in_block);

    const std::uint64_t new_offset = offset + jump;

    offset = new_offset;
    return true;
}

bool can_read_range(std::uint64_t offset, std::size_t read_size, std::uint64_t file_size)
{
    if (offset > file_size)
        return false;

    const std::uint64_t size64 = static_cast<std::uint64_t>(read_size);

    if (size64 > file_size - offset)
        return false;


    return true;
}

bool offset_boundary_check(std::uint64_t offset, std::uint64_t file_size)
{
    if (offset > file_size)
        return false;

    return true;
}