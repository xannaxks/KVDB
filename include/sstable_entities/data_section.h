#include <cstdint>
#include "status.h"
#include "file_helpers.h"
#include "crc32_helpers.h"
#include "file.h"
#include "sstable_entities.h"
#include "record.h"
#include "arena.h"
#include "sstable_entities/index_section.h"

namespace SSTableEntities
{

	struct DataSection
	{
		struct Header {
			Header() noexcept = default;
			Header(const Header& other) noexcept = default;
			Header(Header& other) noexcept = default;
			Header(Header&& other) noexcept = default;

			BlockType type;
			std::uint32_t payload_disk_size;
			std::uint32_t crc32;

			static std::size_t disk_size();

			//static Result<Header> load(std::unique_ptr<ReadableFile>* file, std::uint64_t& offset);
			Status write(WritableFile& file, std::uint64_t& offset);

			Header& operator=(Header& other) = default;
			Header& operator=(Header&& other) = default;
		};

		struct Payload {
			Payload() noexcept = default;
			Payload(const Payload& other) noexcept = default;
			Payload(Payload& other) noexcept = default;
			Payload(Payload&& other) noexcept = default;
			Payload(const InternalRecord& record) noexcept;

			std::uint32_t key_size;
			std::uint32_t value_size;
			::Type type;
			std::uint32_t flags;
			std::uint32_t reserved;
			std::uint64_t seq_num;
			void* key_ptr;
			void* value_ptr;

			std::size_t disk_size();
			std::size_t disk_size() const;

			//static Result<Payload> load(ReadableFile& file, std::uint64_t& offset, std::uint32_t* must_be_crc = nullptr);
			Status write(WritableFile& file, std::uint64_t& offset);

			static std::size_t fixed_part_disk_size();

			void calculate_crc32(std::uint32_t& crc_buffer);
			void append_crc32(std::uint32_t& crc_buffer);

			Payload& operator=(Payload& other) = default;
			Payload& operator=(Payload&& other) = default;
		};
		struct DataBlock
		{
			DataBlock() noexcept = default;
			DataBlock(const DataBlock& other) noexcept = default;
			DataBlock(DataBlock& other) noexcept = default;
			DataBlock(DataBlock&& other) noexcept = default;

			Header header;
			std::vector<Payload> payloads;

			std::size_t disk_size() const;
			std::size_t disk_size();

			bool can_payload_fit(Payload& payload);
			void add_payload(Payload& payload);

			Status write(WritableFile& file, std::uint64_t& offset, IndexSection& index_section);
			//static Result<DataBlock> load(std::unique_ptr<ReadableFile>* file, std::uint64_t& offset);

			void calculate_crc32(std::uint32_t& crc_buffer);

			DataBlock& operator=(DataBlock& other) = default;
			DataBlock& operator=(DataBlock&& other) = default;
		};
		DataSection() noexcept = default;
		DataSection(const DataSection& other) noexcept = default;
		DataSection(DataSection& other) noexcept = default;
		DataSection(DataSection&& other) noexcept = default;

		std::vector<DataBlock> data_blocks;

		void init_new_block();
		Status add_payload(const InternalRecord& record);

		std::size_t disk_size();

		Status write(WritableFile& file, std::uint64_t& data_block_offset, IndexSection& index_section, std::uint64_t& data_offset);
		//static Result<DataSection> load(
		//	std::unique_ptr<ReadableFile>* file,
		//	std::uint64_t& offset,
		//	const std::uint64_t& first_data_block_offset,
		//	std::uint32_t data_block_count
		//);

		DataSection& operator=(DataSection& other) = default;
		DataSection& operator=(DataSection&& other) = default;
	};
}