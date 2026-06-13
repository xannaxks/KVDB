#include "status.h"
#include "arena.h"
//#include "sstable.h"
#include <cstdint>
#include "file.h"
#include "file_helpers.h"
#include "endian_io.h"
#include "crc32_helpers.h"
#include "sstable_entities.h"
#include "data_section.h"

namespace SSTableEntities
{
	struct DataSectionView
	{
		struct Header
		{
			Header() noexcept = default;
			Header(const Header& other) noexcept = default;
			Header(Header& other) noexcept = default;
			Header(Header&& other) noexcept = default;

			DataSection::Header header{};

			std::uint64_t header_offset{};
			std::uint64_t payload_offset{};
			std::uint64_t next_block_offset{};

			static Result<Header> load(ReadableFile& file, std::uint64_t& offset);

			Header& operator=(Header& other) = default;
			Header& operator=(Header&& other) = default;
		};
		struct DataBlock
		{
			DataBlock() noexcept = default;
			DataBlock(const DataBlock& other) noexcept = default;
			DataBlock(DataBlock& other) noexcept = default;
			DataBlock(DataBlock&& other) noexcept = default;

			Header header_view{};

			static Result<DataBlock> load(ReadableFile& file, std::uint64_t& offset);

			DataBlock& operator=(DataBlock& other) = default;
			DataBlock& operator=(DataBlock&& other) = default;
		};

		DataSectionView() noexcept = default;
		DataSectionView(const DataSectionView& other) noexcept = default;
		DataSectionView(DataSectionView& other) noexcept = default;
		DataSectionView(DataSectionView&& other) noexcept = default;

		std::vector<DataBlock> data_blocks{};

		static Result<DataSectionView> load(
			ReadableFile& file,
			std::uint64_t& offset,
			const std::uint64_t& first_data_block_offset,
			std::uint32_t data_block_count
		);

		DataSectionView& operator=(DataSectionView& other) = default;
		DataSectionView& operator=(DataSectionView&& other) = default;
	};
}