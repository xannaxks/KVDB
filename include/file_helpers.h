#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include "status.h"
#include "file.h"


Status fits_in_block(std::uint64_t offset,
    std::size_t size,
    const std::uint32_t BLOCK_SIZE);


Status ensure_fits_in_block(WritableFile& file,
    std::size_t size,
    std::uint64_t& offset,
    const std::uint32_t BLOCK_SIZE);

Status align_to_block_boundary(WritableFile& file,
    std::uint64_t& offset,
    const std::uint32_t BLOCK_SIZE);


Status ensure_fits_in_block(ReadableFile& file,
    std::size_t size,
    std::uint64_t& offset,
    const std::uint32_t BLOCK_SIZE);

Status align_to_block_boundary(ReadableFile& file,
    std::uint64_t& offset,
    const std::uint32_t BLOCK_SIZE);

Status move_g_to_next_block(ReadableFile& file,
    std::uint64_t& offset,
    const std::uint32_t BLOCK_SIZE);

Status can_read_range(std::uint64_t offset, std::size_t read_size, std::uint64_t file_size);

Status offset_boundary_check(std::uint64_t offset, std::uint64_t file_size);