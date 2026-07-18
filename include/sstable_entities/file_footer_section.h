#pragma once

#include "sstable_entities.h"
#include "arena.h"
#include "status.h"
#include "file.h"
#include "file_helpers.h"
#include "endian_io.h"
#include "crc32_helpers.h"
#include "sstable_entities/data_section.h"
#include "sstable_entities/bloom_section.h"
#include "sstable_entities/index_section.h"
#include "sstable_entities/meta_section.h"
#include <cstddef>
#include <cstdint>
#include <format>

namespace SSTableEntities
{
	struct FileFooterSection
	{
		FileFooterSection();
		FileFooterSection(const FileFooterSection& other) noexcept = default;
		FileFooterSection(FileFooterSection& other) noexcept = default;
		FileFooterSection(FileFooterSection&& other) noexcept = default;

		std::uint32_t magic;
		std::uint32_t version;
		std::uint32_t reserved;

		std::uint64_t data_offset;
		std::uint64_t data_block_count;

		std::uint64_t index_offset;
		std::uint32_t index_size;

		std::uint64_t bloom_offset;
		std::uint32_t bloom_size;

		std::uint64_t meta_offset;
		std::uint32_t meta_size;

		std::uint64_t file_size;
		std::uint32_t footer_crc32;

		static std::size_t disk_size();

		void finalize(WritableFile& file, std::uint64_t offset);
		static Result<FileFooterSection> load(ReadableFile& file, std::uint64_t& offset, std::uint64_t file_footer_backwards_offset);
		void rebuild(IndexSection& index_block, std::uint64_t index_offset);
		void rebuild(BloomSection& bloom_block, std::uint64_t bloom_offset);
		void rebuild(MetaSection& meta_block, std::uint64_t meta_offset);

		Status write(WritableFile& file, std::uint64_t& offset);
		//static Result<FileFooterSection> load(
		//	ReadableFile& file,
		//	std::uint64_t& offset,
		//	std::uint64_t file_footer_backwards_offset = 0,
		//);

		void calculate_crc32(std::uint32_t& crc_buffer);

		FileFooterSection& operator=(FileFooterSection& other) = default;
		FileFooterSection& operator=(FileFooterSection&& other) = default;
	};
}