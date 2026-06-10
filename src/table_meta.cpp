#include "table_meta.h"
#include <utility>

using namespace SSTableEntities;

Result<TableMeta> make_table_meta(
	const SSTable& sstable,
	std::uint32_t level,
	Arena& arena
)
{
	TableMeta meta{};
	const FileHeaderSection& file_header_section = sstable.get_file_header_section();
	const MetaSection& meta_section = sstable.get_meta_section();
	const FileFooterSection& file_footer_section = sstable.get_file_footer_section();

	meta.table_id = file_header_section.table_id;
	meta.level = level;
	meta.path = sstable.get_path();

	auto smallest = ArenaEntry::make_entry(
		arena,
		std::span<const std::byte>(
			static_cast<const std::byte*>(meta_section.payload.min_key_ptr),
			meta_section.payload.min_key_size
		)
	);
	if (!smallest.is_ok())
		return Result<TableMeta>::fail(std::move(smallest.status));

	auto largest = ArenaEntry::make_entry(
		arena,
		std::span<const std::byte>(
			static_cast<const std::byte*>(meta_section.payload.max_key_ptr),
			meta_section.payload.max_key_size
		)
	);
	if (!largest.is_ok())
		return Result<TableMeta>::fail(std::move(largest.status));

	meta.record_count = meta_section.payload.record_count;
	meta.tombstone_count = meta_section.payload.tombstone_count;
	meta.min_seq = meta_section.payload.min_seq_num;
	meta.max_seq = meta_section.payload.max_seq_num;
	meta.data_block_count = meta_section.payload.data_block_count;
	meta.data_bytes = meta_section.payload.data_bytes;

	meta.file_size = file_footer_section.file_size;

	return Result<TableMeta>::ok(std::move(meta));
}