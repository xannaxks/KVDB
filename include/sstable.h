#pragma once

#ifdef _WIN32

	#ifndef NOMINMAX
		#define NOMINMAX
	#endif

	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif

#endif

#include <format>
#include "mem_table.h"
#include <fstream>
#include <string>
#include <format>
#include "record.h"
#include <memory>
#include <algorithm>
#include "MurmurHash3.h"
#include <filesystem>
#include <algorithm>
#include <limits>
#include <format>
#include "status.h"
#include "file.h"
#include <stdexcept>
#include <optional>
#include "mem_table.h"
#include "sstable_entities.h"
#include "sstable_entities/file_header_section.h"
#include "sstable_entities/data_section.h"
#include "sstable_entities/data_section_view.h"
#include "sstable_entities/index_section.h"
#include "sstable_entities/bloom_section.h"
#include "sstable_entities/meta_section.h"
#include "sstable_entities/file_footer_section.h"
#include "file_helpers.h"
#include "crc32_helpers.h"
//#include "zlib.h"
#include <bitset>
#include <cstdint>
#include "endian_io.h"

// loading correction

class SSTable
{
public:
	SSTable() = default;
	explicit SSTable( std::filesystem::path& path,  std::filesystem::path& final_path);
	explicit SSTable(std::filesystem::path& path);

	SSTable(const SSTable&) = delete;
	SSTable& operator=(const SSTable&) = delete;

	SSTable(SSTable&&) noexcept = default;
	SSTable& operator=(SSTable&&) noexcept = default;

	//SSTable(const MemTable& mem_table, std::uint32_t id);

private:

	std::filesystem::path path;
	std::filesystem::path final_path;

	std::unique_ptr<ReadableFile> file_in;
	std::unique_ptr<WritableFile> file_out;
	SSTableEntities::FileHeaderSection file_header_section{};
	SSTableEntities::DataSection data_section{};
	SSTableEntities::DataSectionView data_section_view{};
	SSTableEntities::IndexSection index_section{};
	SSTableEntities::BloomSection bloom_section{};
	SSTableEntities::MetaSection meta_section{};
	SSTableEntities::FileFooterSection file_footer_section{};

	Status write();
	static Result<SSTable> load( std::filesystem::path& path, Arena& arena);
//#ifdef _WIN32
	Status fsync(WritableFile& file_out);

	friend class SSTableManager;
	friend class SSTableWriter;
	friend class SSTableLoader;
	friend class SSTableIterator;

public:

	const std::filesystem::path& get_path() const;
	const std::filesystem::path& get_final_path() const;

	const SSTableEntities::FileHeaderSection& get_file_header_section() const;
	const SSTableEntities::DataSection& get_data_section() const;
	const SSTableEntities::DataSectionView& get_data_section_view() const;
	const SSTableEntities::IndexSection& get_index_section() const;
	const SSTableEntities::BloomSection& get_bloom_section() const;
	const SSTableEntities::MetaSection& get_meta_section() const;
	const SSTableEntities::FileFooterSection& get_file_footer_section() const;

	const std::filesystem::path& get_path() const;

	const std::filesystem::path& get_final_path() const;

};
