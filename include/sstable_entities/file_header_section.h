#include <cstdint>
#include "status.h"
#include "file_helpers.h"
#include "crc32_helpers.h"
#include "file.h"
#include "sstable_entities.h"

namespace SSTableEntities
{
	struct FileHeaderSection
	{
		FileHeaderSection() = default;
		FileHeaderSection(std::uint32_t table_id);
		FileHeaderSection(const FileHeaderSection& other) noexcept = default;
		FileHeaderSection(FileHeaderSection& other) noexcept = default;
		FileHeaderSection(FileHeaderSection&& other) noexcept = default;

		std::uint32_t magic;
		std::uint32_t version;
		std::uint32_t flags;
		std::uint32_t block_size;
		std::uint32_t table_id;
		std::uint32_t crc32;

		static std::size_t disk_size();

		Status write(WritableFile& file, std::uint64_t& offset);
		static Result<FileHeaderSection> load(ReadableFile& file, std::uint64_t& offset);

		Status calculate_crc32(std::uint32_t& crc_buffer);

		FileHeaderSection& operator=(FileHeaderSection& other) = default;
		FileHeaderSection& operator=(FileHeaderSection&& other) = default;
	};
}