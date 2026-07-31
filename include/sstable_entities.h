/**
 * @file sstable_entities.h
 * @brief Shared constants and section types for SSTable format version 1.
 *
 * The physical layout is:
 * @code
 * file header | aligned data blocks | index | bloom | metadata | file footer
 * @endcode
 *
 * Offsets and sizes in the footer provide bounded random access to the variable
 * sections; each section also carries its own validation metadata.
 */
#pragma once

#include <cstdint>

namespace SSTableEntities
{
	constexpr std::uint32_t FILE_HEADER_MAGIC = 0x53535431; // SST1
	constexpr std::uint32_t FILE_FOOTER_MAGIC = 0x46545231; // FTR1
	constexpr std::uint32_t LATEST_SSTABLE_VERSION = 1;
	constexpr std::uint32_t BLOCK_SIZE = 4096;
	constexpr std::uint32_t BLOOM_HASH_COUNT = 2;
	constexpr std::uint32_t BLOOM_MASK_BIT_SIZE = 128; // number of byte-addressed Bloom slots

	struct FileHeaderSection;
	struct DataSection;
	struct DataSectionView;
	struct IndexSection;
	struct BloomSection;
	struct MetaSection;
	struct FileFooterSection;

	/** @brief On-disk discriminator for variable SSTable sections. */
	enum class BlockType : std::uint8_t
	{
		Data = 1, ///< Sorted versioned record payloads.
		Index = 2, ///< Data-block offsets and inclusive key bounds.
		Bloom = 3, ///< Probabilistic negative-lookup filter.
		Meta = 4 ///< Aggregate table statistics and bounds.
	};

}
