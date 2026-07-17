#define NOMINMAX
#include "file_helpers.h"

#include <cassert>
#include <cstdint>
#include <format>
#include <cstddef>
#include <fstream>
#include <algorithm>
#include <vector>
#include <limits>

Status fits_in_block(std::uint64_t offset,
    std::size_t size,
    std::uint32_t block_size)
{
    if (block_size == 0)
        return Status{ StatusCode::InvalidArgument, "block size can't be equal to zero" };

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
    if (block_size == 0)
        return Status{ StatusCode::InvalidArgument, "block size can't be equal to zero" };
    
	Result<std::uint64_t> get_position_result = file.current_position();
    if(!get_position_result.is_ok())
        return get_position_result.status;

    if (offset != get_position_result.value)
        return Status{ StatusCode::InvalidOffset, "manually tracked offset is not equal to file cursor" };

    const std::uint64_t in_block = offset % block_size;

    if (in_block == 0)
        return Status::ok();

    std::uint64_t pad = block_size - in_block;

    std::vector<char> poison(static_cast<std::size_t>(std::min(4096ull, pad)),
        static_cast<char>(0xCD));

    Status status;

    while (pad)
    {
        std::size_t sz = std::min(pad, poison.size());

        status = file.append(poison.data(), sz, offset);
        if (!status.is_ok())
            return status;

        pad -= sz;
    }
    
    return Status::ok();
}

Status ensure_fits_in_block(WritableFile& file,
    std::size_t size,
    std::uint64_t& offset,
    std::uint32_t block_size)
{
    if (block_size == 0)
        return Status{ StatusCode::InvalidArgument, "block size can't be equal to zero" };

    Result<std::uint64_t> get_position_result = file.current_position();
    if (!get_position_result.is_ok())
        return get_position_result.status;

    if (offset != get_position_result.value)
        return Status{ StatusCode::InvalidOffset, "manually tracked offset is not equal to file cursor" };

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
    if (block_size == 0)
        return Status{ StatusCode::InvalidArgument, "block size can't be equal to zero" };

    const std::uint64_t in_block = offset % block_size;

    if (in_block == 0)
        return Status::ok();

    if (offset > std::numeric_limits<std::uint64_t>::max() - (block_size - in_block))
        return Status{
            StatusCode::DataTypeOverflow,
            std::format(
                "offset + (block_size - in_block) overflow uint64_t: {} + ({} - {})",
                offset,
                block_size,
                in_block
            )
    };

    const std::uint64_t new_offset = offset + (block_size - in_block);
    offset = new_offset;

    return Status::ok();
}


Status ensure_fits_in_block(ReadableFile& file,
    std::size_t size,
    std::uint64_t& offset,
    std::uint32_t block_size)
{
    if (block_size == 0)
        return Status{ StatusCode::InvalidArgument, "block size can't be equal to zero" };

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
    if (block_size == 0)
        return Status{ StatusCode::InvalidArgument, "block size can't be equal to zero" };

    const std::uint64_t in_block = offset % block_size;

    const std::uint64_t jump =
        (in_block == 0) ? block_size : (block_size - in_block);

    if (offset > std::numeric_limits<std::uint64_t>::max() - jump)
        return Status{
            StatusCode::DataTypeOverflow,
            std::format(
                "offset + jump overflows std::uint64_t: {} + {}",
                offset,
                jump
            )
        };

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