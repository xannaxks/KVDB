#include "sstable_entities.h"
#include "arena.h"
#include "status.h"
#include "file.h"
#include "file_helpers.h"
#include "endian_io.h"
#include "crc32_helpers.h"

namespace SSTableEntities
{
	struct MetaSection
	{
		struct Header {
			Header() = default;
			Header(const Header& other) noexcept = default;
			Header(Header& other) noexcept = default;
			Header(Header&& other) noexcept = default;

			BlockType type;
			std::uint32_t payload_size;
			std::uint32_t crc32;

			Status write(WritableFile& file, std::uint64_t& offset);
			static Result<Header> load(ReadableFile& file, std::uint64_t& offset);

			static std::size_t disk_size();
			static std::size_t fixed_disk_size();

			Header& operator=(Header& other) = default;
			Header& operator=(Header&& other) = default;
		};
		struct Payload {
			Payload() = default;
			Payload(const Payload& other) noexcept = default;
			Payload(Payload& other) noexcept = default;
			Payload(Payload&& other) noexcept = default;

			std::uint64_t record_count;
			std::uint64_t tombstone_count;
			std::uint64_t min_seq_num;
			std::uint64_t max_seq_num;

			std::uint32_t min_key_size;
			std::uint32_t max_key_size;

			std::uint64_t data_block_count;
			std::uint64_t data_bytes;

			void* max_key_ptr;
			void* min_key_ptr;

			Status write(WritableFile& file, std::uint64_t& offset);
			static Result<Payload> load(ReadableFile& file, std::uint64_t& offset, Arena& arena, MetaSection::Header& header);

			std::size_t disk_size();
			static std::size_t fixed_disk_size();

			void calculate_crc32(std::uint32_t& crc_buffer);

			Payload& operator=(Payload& other) = default;
			Payload& operator=(Payload&& other) = default;
		};
		MetaSection();
		MetaSection(const MetaSection& other) noexcept = default;
		MetaSection(MetaSection& other) noexcept = default;
		MetaSection(MetaSection&& other) noexcept = default;

		Header header;
		Payload payload;

		std::size_t disk_size();
		static std::size_t fixed_disk_size();

		void rebuild(DataSection& data_section, IndexSection& index_section);

		//void rebuild(DataSection& data_block, IndexSection& index_block);
		Status write(WritableFile& file, std::uint64_t& offset, std::uint64_t& meta_offset);
		static Result<MetaSection> load(
			ReadableFile& file,
			std::uint64_t& offset,
			IndexSection& index_block,
			Arena& arena,
			const std::uint64_t& meta_offset = 0
		);

		void calculate_crc32(std::uint32_t& crc_buffer);

		MetaSection& operator=(MetaSection& other) = default;
		MetaSection& operator=(MetaSection&& other) = default;
	};
}