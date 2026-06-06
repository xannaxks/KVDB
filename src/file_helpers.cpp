#include "file_helpers.h"

#include <cassert>
#include <cstdint>
#include <cstddef>
#include <fstream>
#include <vector>


Status fits_in_block(std::uint64_t offset,
    std::size_t size,
    std::uint32_t block_size)
{
    assert(block_size > 0);

    if (size == 0)
        return Status::ok();

    const std::uint64_t in_block = offset % block_size;

	if (size <= block_size - in_block)
		return Status::ok();
    return Status{StatusCode::SizeExceedsBlockBoundary, "Size exceeds block boundary"};
}


// =========================
// Write helpers
// =========================

Status align_to_block_boundary(WritableFile& file,
    std::uint64_t& offset,
    std::uint32_t block_size)
{
    assert(block_size > 0);
    
	Result<std::uint64_t> get_position_result = file.current_position();
    if(!get_position_result.is_ok())
        return get_position_result.status;

    assert(offset == get_position_result.value);

    const std::uint64_t in_block = offset % block_size;

    if (in_block == 0)
        return Status::ok();

    const std::uint64_t pad = block_size - in_block;

    std::vector<char> poison(static_cast<std::size_t>(pad),
        static_cast<char>(0xCD));

    return file.append(poison.data(), poison.size(), offset);
}


Status ensure_fits_in_block(WritableFile& file,
    std::size_t size,
    std::uint64_t& offset,
    std::uint32_t block_size)
{
    assert(block_size > 0);
    Result<std::uint64_t> get_position_result = file.current_position();
    if (!get_position_result.is_ok())
        return get_position_result.status;

    assert(offset == get_position_result.value);

    if(size > block_size)
        return Status{StatusCode::SizeExceedsBlockSize, "Size exceeds block size"};

    Status fits_in_block_result = fits_in_block(offset, size, block_size);

    if (fits_in_block_result.is_ok())
        return Status::ok();

	if (!fits_in_block_result.is_ok() && fits_in_block_result.code != StatusCode::SizeExceedsBlockBoundary)
		return fits_in_block_result;

    return align_to_block_boundary(file, offset, block_size);
}


// =========================
// Read helpers
// =========================

Status align_to_block_boundary(ReadableFile& file,
    std::uint64_t& offset,
    std::uint32_t block_size)
{
    assert(block_size > 0);

    const std::uint64_t in_block = offset % block_size;

    if (in_block == 0)
        return Status::ok();

    const std::uint64_t new_offset = offset + (block_size - in_block);
    offset = new_offset;

    return Status::ok();
}


Status ensure_fits_in_block(ReadableFile& file,
    std::size_t size,
    std::uint64_t& offset,
    std::uint32_t block_size)
{
    assert(block_size > 0);

    if (size > block_size)
        return Status{StatusCode::SizeExceedsBlockSize, "Size exceeds block size"};

    Status fits_in_block_result = fits_in_block(offset, size, block_size);
    if (fits_in_block_result.is_ok())
        return Status::ok();

	if (!fits_in_block_result.is_ok() && fits_in_block_result.code != StatusCode::SizeExceedsBlockBoundary)
		return fits_in_block_result;

    return align_to_block_boundary(file, offset, block_size);
}


Status move_g_to_next_block(ReadableFile& file,
    std::uint64_t& offset,
    std::uint32_t block_size)
{
    assert(block_size > 0);

    const std::uint64_t in_block = offset % block_size;

    const std::uint64_t jump =
        (in_block == 0) ? block_size : (block_size - in_block);

    const std::uint64_t new_offset = offset + jump;

    offset = new_offset;
    return Status::ok();
}

Status can_read_range(std::uint64_t offset, std::size_t read_size, std::uint64_t file_size)
{
    if (offset > file_size)
        return Status{StatusCode::InvalidOffset, "Offset exceeds file size"};

    const std::uint64_t size64 = static_cast<std::uint64_t>(read_size);

    if (size64 > file_size - offset)
        return Status{StatusCode::InvalidReadSize, "Read size exceeds available data"};


    return Status::ok();
}

Status offset_boundary_check(std::uint64_t offset, std::uint64_t file_size)
{
    if (offset > file_size)
        return Status{StatusCode::InvalidOffset, "Offset exceeds file size"};

    return Status::ok();
}