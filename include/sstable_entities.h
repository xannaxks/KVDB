#include <cstdint>

namespace SSTableEntities
{
	constexpr std::uint32_t FILE_HEADER_MAGIC = 0x53535431; // SST1
	constexpr std::uint32_t FILE_FOOTER_MAGIC = 0x46545231; // FTR1
	constexpr std::uint32_t SSTABLE_VERSION = 1;
	constexpr std::uint32_t BLOCK_SIZE = 4096;
	constexpr std::uint32_t BLOOM_HASH_COUNT = 2;
	constexpr std::uint32_t BLOOM_MASK_BIT_SIZE = 128; // amount of bits in bloom mask

	struct FileHeaderSection;
	struct DataSection;
	struct DataSectionView;
	struct IndexSection;
	struct BloomSection;
	struct MetaSection;
	struct FileFooterSection;

	enum class BlockType : std::uint8_t
	{
		Data = 1,
		Index = 2,
		Bloom = 3,
		Meta = 4
	};

}