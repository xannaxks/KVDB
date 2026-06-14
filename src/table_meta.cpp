#include "table_meta.h"
#include <utility>
#include "crc32_helpers.h"
#include "endian_io.h"
#include <format>

using namespace SSTableEntities;

void TableMeta::calculate_crc(std::uint32_t& crc_buffer, bool init = false)
{
	if (init)
		crc_buffer = ::crc32(0, Z_NULL, NULL);

	crc32_add_pod<std::uint64_t>(crc_buffer, this->table_id);
	crc32_add_pod<std::uint32_t>(crc_buffer, this->level);
	crc32_add_pod<std::uint64_t>(crc_buffer, this->min_seq);
	crc32_add_pod<std::uint64_t>(crc_buffer, this->max_seq);
	crc32_add_pod<std::uint64_t>(crc_buffer, this->file_size);
	crc32_add_pod<std::uint64_t>(crc_buffer, this->record_count);
	crc32_add_pod<std::uint64_t>(crc_buffer, this->tombstone_count);
	crc32_add_pod<std::uint32_t>(crc_buffer, this->data_block_count);
	crc32_add_pod<std::uint32_t>(crc_buffer, this->data_bytes);

	crc32_add_pod<std::uint32_t>(crc_buffer, this->smallest_key.size);
	crc32_add_pod<std::uint32_t>(crc_buffer, this->largest_key.size);

	compute_crc32(crc_buffer, smallest_key.data, smallest_key.size);
	compute_crc32(crc_buffer, largest_key.data, largest_key.size);
}

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

Status TableMeta::write(WritableFile& file, std::uint64_t& offset)
{
	Result<std::uint64_t> current_position = file.current_position();
	if (!current_position.is_ok())
		return std::move(current_position.status);
	if (current_position.value != offset)
		return Status{
			StatusCode::InvalidOffset,
			std::format(
				"writable file current offset: {} and tracked offset {} differ",
				current_position.value,
				offset
			)
		};

	Status write_endian_result;

	write_endian_result = kvdb::blockio::write_u64_t_le(file, this->table_id, offset, MANIFEST_BLOCK_SIZE);
	if (!write_endian_result.is_ok())
		return std::move(write_endian_result);
	
	write_endian_result = kvdb::blockio::write_u32_t_le(file, this->level, offset, MANIFEST_BLOCK_SIZE);
	if (!write_endian_result.is_ok())
		return std::move(write_endian_result);

	write_endian_result = kvdb::blockio::write_u64_t_le(file, this->min_seq, offset, MANIFEST_BLOCK_SIZE);
	if (!write_endian_result.is_ok())
		return std::move(write_endian_result);

	write_endian_result = kvdb::blockio::write_u64_t_le(file, this->max_seq, offset, MANIFEST_BLOCK_SIZE);
	if (!write_endian_result.is_ok())
		return std::move(write_endian_result);

	write_endian_result = kvdb::blockio::write_u64_t_le(file, this->file_size, offset, MANIFEST_BLOCK_SIZE);
	if (!write_endian_result.is_ok())
		return std::move(write_endian_result);

	write_endian_result = kvdb::blockio::write_u64_t_le(file, this->record_count, offset, MANIFEST_BLOCK_SIZE);
	if (!write_endian_result.is_ok())
		return std::move(write_endian_result);

	write_endian_result = kvdb::blockio::write_u64_t_le(file, this->tombstone_count, offset, MANIFEST_BLOCK_SIZE);
	if (!write_endian_result.is_ok())
		return std::move(write_endian_result);

	write_endian_result = kvdb::blockio::write_u64_t_le(file, this->data_block_count, offset, MANIFEST_BLOCK_SIZE);
	if (!write_endian_result.is_ok())
		return std::move(write_endian_result);

	write_endian_result = kvdb::blockio::write_u64_t_le(file, this->data_bytes, offset, MANIFEST_BLOCK_SIZE);
	if (!write_endian_result.is_ok())
		return std::move(write_endian_result);

	write_endian_result = kvdb::blockio::write_u64_t_le(file, this->smallest_key.size, offset, MANIFEST_BLOCK_SIZE);
	if (!write_endian_result.is_ok())
		return std::move(write_endian_result);

	write_endian_result = kvdb::blockio::write_u64_t_le(file, this->largest_key.size, offset, MANIFEST_BLOCK_SIZE);
	if (!write_endian_result.is_ok())
		return std::move(write_endian_result);

	write_endian_result = kvdb::blockio::write_bytes(
		file, 
		std::span<std::byte>(reinterpret_cast<std::byte*>(this->smallest_key.data), this->smallest_key.size),
		offset,
		MANIFEST_BLOCK_SIZE
	);
	if (!write_endian_result.is_ok())
		return std::move(write_endian_result);

	write_endian_result = kvdb::blockio::write_bytes(
		file,
		std::span<std::byte>(reinterpret_cast<std::byte*>(this->largest_key.data), this->largest_key.size),
		offset,
		MANIFEST_BLOCK_SIZE
	);
	if (!write_endian_result.is_ok())
		return std::move(write_endian_result);

	return Status::ok();
}

Result<TableMeta> TableMeta::load(ReadableFile& file, std::uint64_t& offset, Arena& arena)
{
	Status read_endian_result;
	TableMeta result{};

	read_endian_result = kvdb::blockio::read_u64_t_le(file, result.table_id, offset, MANIFEST_BLOCK_SIZE);
	if (!read_endian_result.is_ok())
		return Result<TableMeta>::fail(std::move(read_endian_result));

	read_endian_result = kvdb::blockio::read_u32_t_le(file, result.level, offset, MANIFEST_BLOCK_SIZE);
	if (!read_endian_result.is_ok())
		return Result<TableMeta>::fail(std::move(read_endian_result));
	
	read_endian_result = kvdb::blockio::read_u64_t_le(file, result.min_seq, offset, MANIFEST_BLOCK_SIZE);
	if (!read_endian_result.is_ok())
		return Result<TableMeta>::fail(std::move(read_endian_result));
	
	read_endian_result = kvdb::blockio::read_u64_t_le(file, result.max_seq, offset, MANIFEST_BLOCK_SIZE);
	if (!read_endian_result.is_ok())
		return Result<TableMeta>::fail(std::move(read_endian_result));

	read_endian_result = kvdb::blockio::read_u64_t_le(file, result.file_size, offset, MANIFEST_BLOCK_SIZE);
	if (!read_endian_result.is_ok())
		return Result<TableMeta>::fail(std::move(read_endian_result));

	read_endian_result = kvdb::blockio::read_u64_t_le(file, result.record_count, offset, MANIFEST_BLOCK_SIZE);
	if (!read_endian_result.is_ok())
		return Result<TableMeta>::fail(std::move(read_endian_result));

	read_endian_result = kvdb::blockio::read_u64_t_le(file, result.tombstone_count, offset, MANIFEST_BLOCK_SIZE);
	if (!read_endian_result.is_ok())
		return Result<TableMeta>::fail(std::move(read_endian_result));

	read_endian_result = kvdb::blockio::read_u64_t_le(file, result.data_block_count, offset, MANIFEST_BLOCK_SIZE);
	if (!read_endian_result.is_ok())
		return Result<TableMeta>::fail(std::move(read_endian_result));

	read_endian_result = kvdb::blockio::read_u64_t_le(file, result.data_bytes, offset, MANIFEST_BLOCK_SIZE);
	if (!read_endian_result.is_ok())
		return Result<TableMeta>::fail(std::move(read_endian_result));

	read_endian_result = kvdb::blockio::read_u32_t_le(file, result.smallest_key.size, offset, MANIFEST_BLOCK_SIZE);
	if (!read_endian_result.is_ok())
		return Result<TableMeta>::fail(std::move(read_endian_result));

	read_endian_result = kvdb::blockio::read_u32_t_le(file, result.largest_key.size, offset, MANIFEST_BLOCK_SIZE);
	if (!read_endian_result.is_ok())
		return Result<TableMeta>::fail(std::move(read_endian_result));

	if (result.smallest_key.size > MANIFEST_BLOCK_SIZE - sizeof(result.smallest_key.size))
		return Result<TableMeta>::fail(
			Status{
				StatusCode::AllocationTooLarge, 
				std::format(
					"smallest key size {} exceeds manifest block size {}",
					result.smallest_key.size,
					MANIFEST_BLOCK_SIZE
				)
			}
		);

	if (result.largest_key.size > MANIFEST_BLOCK_SIZE - sizeof(result.largest_key.size))
	{
		return Result<TableMeta>::fail(
			Status{
				StatusCode::AllocationTooLarge,
				std::format(
					"largest key size {} exceeds manifest block size {}",
					result.largest_key.size,
					MANIFEST_BLOCK_SIZE
				)
			}
		);
	}

	Result<void*> alloc_result;
	
	alloc_result = arena.alloc(result.smallest_key.size);
	if (!alloc_result.is_ok())
		return Result<TableMeta>::fail(std::move(alloc_result.status));

	result.smallest_key.data = alloc_result.value;

	alloc_result = arena.alloc(result.largest_key.size);
	if (!alloc_result.is_ok())
		return Result<TableMeta>::fail(std::move(alloc_result.status));

	result.largest_key.data = alloc_result.value;

	read_endian_result = kvdb::blockio::read_bytes(
		file,
		reinterpret_cast<std::byte*>(result.smallest_key.data),
		result.smallest_key.size,
		offset,
		MANIFEST_BLOCK_SIZE
	);
	if (!read_endian_result.is_ok())
		return Result<TableMeta>::fail(std::move(read_endian_result));

	read_endian_result = kvdb::blockio::read_bytes(
		file,
		reinterpret_cast<std::byte*>(result.largest_key.data),
		result.largest_key.size,
		offset,
		MANIFEST_BLOCK_SIZE
	);
	if (!read_endian_result.is_ok())
		return Result<TableMeta>::fail(std::move(read_endian_result));

	return Result<TableMeta>::ok(std::move(result));
}