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

namespace SSTableEntities
{
	struct BloomSection
	{
		struct Header {
			Header() = default;
			Header(const Header& other) noexcept = default;
			Header(Header& other) noexcept = default;
			Header(Header&& other) noexcept = default;

			BlockType type = BlockType::Bloom; // std::uint8_t
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

			std::uint64_t bloom_bits = 0;
			std::uint32_t hash_count = 0;
			std::uint32_t key_count = 0;
			std::vector<std::uint8_t> mask;

			Status write(WritableFile& file, std::uint64_t& offset);
			static Result<Payload> load(ReadableFile& file, std::uint64_t& offset);

			static std::size_t disk_size();

			void calculate_crc32(std::uint32_t& crc_buffer);

			Payload& operator=(Payload& other) = default;
			Payload& operator=(Payload&& other) = default;
		};
		BloomSection();
		BloomSection(const BloomSection& other) noexcept = default;
		BloomSection(BloomSection& other) noexcept = default;
		BloomSection(BloomSection&& other) noexcept = default;

		Header header;
		Payload payload;

		static std::size_t disk_size();

		//void rebuild(DataSection& data_block);
		Status write(WritableFile& file, std::uint64_t& offset, std::uint64_t& bloom_offset);
		static Result<BloomSection> load(ReadableFile& file, std::uint64_t& offset, const std::uint64_t& bloom_offset);

		void add_key(const void* key_ptr, std::uint32_t key_size);
		void rebuild(const DataSection& data_section);
		void recompute_crc32();

		bool may_contain(const void* key_ptr, std::uint32_t key_size) const;

		void calculate_crc32(std::uint32_t& crc_buffer);

		BloomSection& operator=(BloomSection& other) = default;
		BloomSection& operator=(BloomSection&& other) = default;
	};
}