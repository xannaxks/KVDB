#pragma once

#include "sstable_entities.h"
#include "arena.h"
#include "status.h"
#include "file.h"
#include "file_helpers.h"
#include "endian_io.h"
#include "crc32_helpers.h"
#include <cstddef>
#include <cstdint>
#include <vector>
#include <format>

namespace SSTableEntities
{
	struct IndexSection
	{
		struct Header {
			Header() = default;
			Header(const Header& other) noexcept = default;
			Header(Header& other) noexcept = default;
			Header(Header&& other) noexcept = default;

			BlockType type = BlockType::Index;
			std::uint32_t payload_size = 0;
			std::uint32_t crc32 = 0;

			Status write(WritableFile& file, std::uint64_t& offset);
			static Result<Header> load(ReadableFile& file, std::uint64_t& offset);

			static std::size_t disk_size();

			Header& operator=(Header& other) = default;
			Header& operator=(Header&& other) = default;
		};
		struct Payload {
			Payload() = default;
			Payload(const Payload& other) noexcept = default;
			Payload(Payload& other) noexcept = default;
			Payload(Payload&& other) noexcept = default;

			std::uint64_t data_block_offset = 0;
			std::uint32_t first_key_size = 0;
			std::uint32_t last_key_size = 0;
			void* first_key_ptr = nullptr;
			void* last_key_ptr = nullptr;

			Status write(WritableFile& file, std::uint64_t& offset);
			static Result<Payload> load(ReadableFile& file, std::uint64_t& offset, Arena& arena);

			std::size_t disk_size();
			static std::size_t fixed_disk_size();

			void calculate_crc32(std::uint32_t& crc_buffer);
			void append_crc32(std::uint32_t& crc_buffer);

			Payload& operator=(Payload& other) = default;
			Payload& operator=(Payload&& other) = default;
		};
		IndexSection();
		IndexSection(const IndexSection& other) noexcept = default;
		IndexSection(IndexSection& other) noexcept = default;
		IndexSection(IndexSection&& other) noexcept = default;

		static std::size_t fixed_disk_size();

		Header header;
		std::vector<Payload> payloads;

		std::size_t disk_size();

		//void rebuild(std::vector<std::tuple<DataSection::Payload*, DataSection::Payload*, std::uint64_t, std::uint64_t>>& index_boundaries);
		void add_index(std::uint64_t data_block_offset, std::uint32_t first_key_size, std::uint32_t last_key_size, void* first_key_ptr, void* last_key_ptr);

		Status write(WritableFile& file, std::uint64_t& offset, std::uint64_t& index_offset);
		static Result<IndexSection> load(
			ReadableFile& file,
			std::uint64_t& offset,
			Arena& arena,
			const std::uint64_t& index_offset
		);

		void calculate_crc32(std::uint32_t& crc_buffer);

		IndexSection& operator=(IndexSection& other) = default;
		IndexSection& operator=(IndexSection&& other) = default;
	};
}