#include "sstable.h"
#include <algorithm>
#include <limits>

#ifdef _WIN32

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif

    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif

#endif

using namespace SSTableEntities;

// bloom hashes

static std::uint64_t fnv1a64(const void* data, std::uint32_t size, std::uint64_t seed)
{
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint64_t h = 14695981039346656037ull ^ seed;

    for (std::uint32_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }

    return h;
}
static std::uint64_t bloom_hash_i(const void* key, std::uint32_t key_size, std::uint32_t i)
{
    std::uint64_t h1 = fnv1a64(key, key_size, 0x9E3779B97F4A7C15ull);
    std::uint64_t h2 = fnv1a64(key, key_size, 0xC2B2AE3D27D4EB4Full);

    return h1 + i * h2;
}

/// can fit


bool DataSection::DataBlock::can_payload_fit(DataSection::Payload& payload)
{
    return this->disk_size() + payload.disk_size() <= BLOCK_SIZE;
}


/// init new block


void DataSection::init_new_block()
{
    data_blocks.emplace_back(DataSection::Header());
}


/// disk_size


std::size_t FileHeaderSection::disk_size()
{
    return (
        sizeof(magic) +
        sizeof(version) +
        sizeof(flags) +
        sizeof(block_size) +
        sizeof(table_id) +
        sizeof(crc32)
        );
}

std::size_t DataSection::Header::disk_size()
{
    return (
        sizeof(type) +
        sizeof(payload_disk_size) +
        sizeof(crc32)
        );
}
std::size_t DataSection::Payload::disk_size()
{
    return (
        sizeof(key_size) +
        sizeof(value_size) +
        key_size +
        value_size +
        sizeof(type) +
        sizeof(flags) +
        sizeof(reserved) +
        sizeof(seq_num)
        );
}
std::size_t DataSection::Payload::disk_size() const
{
    return (
        sizeof(key_size) +
        sizeof(value_size) +
        key_size +
        value_size +
        sizeof(type) +
        sizeof(flags) +
        sizeof(reserved) +
        sizeof(seq_num)
        );
}
std::size_t DataSection::Payload::fixed_part_disk_size()
{
    return sizeof(key_size) + sizeof(value_size) + sizeof(type) + sizeof(flags) + sizeof(reserved) + sizeof(seq_num);
}
std::size_t DataSection::disk_size()
{
    std::size_t cnt = 0;
    for (auto& block : data_blocks)
        cnt += block.disk_size();
    return cnt;
}
std::size_t DataSection::DataBlock::disk_size()
{
    std::size_t res = Header::disk_size();
    for (auto& payload : this->payloads)
        res += payload.disk_size();
    return res;
}
std::size_t DataSection::DataBlock::disk_size() const
{
    std::size_t res = Header::disk_size();
    for (auto& payload : this->payloads)
        res += payload.disk_size();
    return res;
}

std::size_t IndexSection::Header::disk_size()
{
    return (
        sizeof(type) +
        sizeof(payload_size) +
        sizeof(crc32)
        );
}
std::size_t IndexSection::Payload::disk_size()
{
    return (
        sizeof(first_key_size) +
        sizeof(last_key_size) +
        first_key_size +
        last_key_size +
        sizeof(data_block_offset)
        );
}
std::size_t IndexSection::Payload::fixed_disk_size()
{
    return (
        sizeof(data_block_offset) + 
        sizeof(first_key_size) + 
        sizeof(last_key_size)
        );
}
std::size_t IndexSection::disk_size()
{
    std::size_t cnt = 0;
    for (auto& i : payloads)
        cnt += i.disk_size();
    return (
        Header::disk_size() + cnt
        );
}

std::size_t BloomSection::disk_size()
{
    return Header::disk_size() + Payload::disk_size();
}
std::size_t BloomSection::Header::disk_size()
{
    return (
        sizeof(type) +
        sizeof(payload_size) +
        sizeof(crc32)
        );
}
std::size_t BloomSection::Payload::disk_size()
{
    return (
        sizeof(bloom_bits) +
        sizeof(hash_count) +
        sizeof(key_count) +
        BLOOM_MASK_BIT_SIZE
        );
}

std::size_t MetaSection::fixed_disk_size()
{
    return Header::fixed_disk_size() + Payload::fixed_disk_size();
}
std::size_t MetaSection::disk_size()
{
    return Header::disk_size() + this->payload.disk_size();
}
std::size_t MetaSection::Header::fixed_disk_size()
{
    return (
        sizeof(type) +
        sizeof(payload_size) +
        sizeof(crc32)
        );
}
std::size_t MetaSection::Payload::fixed_disk_size()
{
    return (
        sizeof(record_count) +
        sizeof(tombstone_count) +
        sizeof(min_seq_num) +
        sizeof(max_seq_num) + 
        sizeof(min_key_size) +
        sizeof(max_key_size) +
        sizeof(data_block_count) +
        sizeof(data_bytes)
        );
}
std::size_t MetaSection::Payload::disk_size()
{
    return (
        sizeof(record_count) +
        sizeof(tombstone_count) +
        sizeof(min_seq_num) +
        sizeof(max_seq_num) +
        sizeof(min_key_size) +
        sizeof(max_key_size) +
        sizeof(data_block_count) +
        sizeof(data_bytes) + 
        this->min_key_size +
        this->max_key_size
        );
}
std::size_t MetaSection::Header::disk_size()
{
    return (
        sizeof(type) +
        sizeof(payload_size) +
        sizeof(crc32)
        );
}

std::size_t FileFooterSection::disk_size()
{
    return (
        sizeof(magic) +
        sizeof(version) +
        sizeof(reserved) +
        sizeof(index_offset) +
        sizeof(index_size) +
        sizeof(bloom_offset) +
        sizeof(bloom_size) +
        sizeof(meta_offset) +
        sizeof(meta_size) +
        sizeof(file_size) +
        sizeof(footer_crc32) +
        sizeof(data_offset) + 
        sizeof(data_block_count)
        );
}

/// helpers


FileHeaderSection::FileHeaderSection(std::uint32_t table_id)
    : magic(FILE_HEADER_MAGIC),
    version(SSTABLE_VERSION),
    flags(0),
    block_size(BLOCK_SIZE),
    table_id(table_id),
    crc32(0)
{
    crc32 = ::crc32(0L, Z_NULL, 0);
    crc32_add_pod<std::uint32_t>(crc32, this->magic);
    crc32_add_pod<std::uint32_t>(crc32, this->version);
    crc32_add_pod<std::uint32_t>(crc32, this->flags);
    crc32_add_pod<std::uint32_t>(crc32, this->block_size);
    crc32_add_pod<std::uint32_t>(crc32, this->table_id);
}
DataSection::Header::Header()
    : type(BlockType::Data), payload_disk_size(0), crc32(::crc32(0L, Z_NULL, 0))
{
}
IndexSection::IndexSection()
{
    header.type = BlockType::Index;
    header.payload_size = 0;
    header.crc32 = ::crc32(0L, Z_NULL, 0);
}
BloomSection::BloomSection()
{
    header.type = BlockType::Bloom;
    header.payload_size = Payload::disk_size();
    header.crc32 = ::crc32(0L, Z_NULL, 0);

    payload.bloom_bits = payload.mask.size();
    payload.hash_count = BLOOM_HASH_COUNT;
    payload.key_count = 0;
    payload.mask.resize(BLOOM_MASK_BIT_SIZE, 0);

    crc32_add_pod<std::uint64_t>(header.crc32, payload.bloom_bits);
    crc32_add_pod<std::uint32_t>(header.crc32, payload.hash_count);
    crc32_add_pod<std::uint32_t>(header.crc32, payload.key_count);

    compute_crc32(header.crc32, payload.mask.data(), payload.mask.size());
}
MetaSection::MetaSection()
{
    header.type = BlockType::Meta;
    header.payload_size = Payload::fixed_disk_size();
    header.crc32 = ::crc32(0L, Z_NULL, 0);

    payload.record_count = 0;
    payload.tombstone_count = 0;
    payload.min_seq_num = 0;
    payload.max_seq_num = 0;
    payload.min_key_size = 0;
    payload.max_key_size = 0;
    payload.data_block_count = 0;
    payload.data_bytes = 0;

    crc32_add_pod<std::uint64_t>(header.crc32, payload.record_count);
    crc32_add_pod<std::uint64_t>(header.crc32, payload.tombstone_count);
    crc32_add_pod<std::uint64_t>(header.crc32, payload.min_seq_num);
    crc32_add_pod<std::uint64_t>(header.crc32, payload.max_seq_num);
    crc32_add_pod<std::uint32_t>(header.crc32, payload.min_key_size);
    crc32_add_pod<std::uint32_t>(header.crc32, payload.max_key_size);
    crc32_add_pod<std::uint64_t>(header.crc32, payload.data_block_count);
    crc32_add_pod<std::uint64_t>(header.crc32, payload.data_bytes);
}
FileFooterSection::FileFooterSection()
    : magic(FILE_FOOTER_MAGIC),
    version(SSTABLE_VERSION),
    reserved(0),
    index_offset(0),
    index_size(0),
    bloom_offset(0),
    bloom_size(0),
    meta_offset(0),
    meta_size(0),
    file_size(0),
    footer_crc32(0)
{
    footer_crc32 = ::crc32(0L, Z_NULL, 0);

    crc32_add_pod<std::uint32_t>(footer_crc32, magic);
    crc32_add_pod<std::uint32_t>(footer_crc32, version);
    crc32_add_pod<std::uint32_t>(footer_crc32, reserved);
    crc32_add_pod<std::uint64_t>(footer_crc32, index_offset);
    crc32_add_pod<std::uint32_t>(footer_crc32, index_size);
    crc32_add_pod<std::uint64_t>(footer_crc32, bloom_offset);
    crc32_add_pod<std::uint32_t>(footer_crc32, bloom_size);
    crc32_add_pod<std::uint64_t>(footer_crc32, meta_offset);
    crc32_add_pod<std::uint32_t>(footer_crc32, meta_size);
    crc32_add_pod<std::uint64_t>(footer_crc32, file_size);
}

SSTable::SSTable(const std::filesystem::path& path)
    : path(path),
    file_header_section(),
    data_section(),
    index_section(),
    bloom_section(),
    meta_section(),
    file_footer_section()
{
}
SSTableWriter::SSTableWriter()
{
}
SSTableLoader::SSTableLoader()
{
}
SSTableManager::SSTableManager()
    : sstable_writer(), sstable_loader()
{
}


/// Adding one more sstable to sstable manager pool


void SSTableManager::add_to_pool(SSTable&& sstable)
{
    this->immutable_pool.emplace_back(std::move(sstable));
}


/// writes SSTable, manager, writer;


void SSTable::write()
{
    file_out = open_writable_file(path);
    if (!file_out)
        throw std::runtime_error("failed to open SSTable for write");

    std::uint64_t offset = 0;

    file_header_section.write(*file_out, offset);

    // Data write also fills index_block
    std::uint64_t data_offset = 0;
    data_section.write(*file_out, offset, index_section, data_offset);

    // Build bloom/meta after data/index are finalized
    bloom_section.rebuild(data_section);
    meta_section.rebuild(data_section, index_section);

    std::uint64_t index_offset = 0;
    std::uint64_t bloom_offset = 0;
    std::uint64_t meta_offset = 0;

    index_section.write(*file_out, offset, index_offset);
    bloom_section.write(*file_out, offset, bloom_offset);
    meta_section.write(*file_out, offset, meta_offset);

    file_footer_section.data_offset = data_offset;
    file_footer_section.data_block_count = data_section.data_blocks.size();

    file_footer_section.index_offset = index_offset;
    file_footer_section.index_size = static_cast<std::uint32_t>(index_section.disk_size());

    file_footer_section.bloom_offset = bloom_offset;
    file_footer_section.bloom_size = static_cast<std::uint32_t>(bloom_section.disk_size());

    file_footer_section.meta_offset = meta_offset;
    file_footer_section.meta_size = static_cast<std::uint32_t>(meta_section.disk_size());

    //file_footer_section.finalize(*file_out, offset);
    //file_footer_section.write(*file_out, offset);

    if (!align_to_block_boundary(*file_out, offset, BLOCK_SIZE))
        ensure_atomicity();

    file_footer_section.finalize(*file_out, offset);

    if (!file_footer_section.write(*file_out, offset))
        ensure_atomicity();

    if (!this->fsync(*file_out))
        ensure_atomicity();

    file_out = nullptr;
}
void SSTableWriter::write(SSTable& sstable)
{
    sstable.write();
}
void SSTableManager::write_latest(bool erase)
{
    if (this->immutable_pool.empty()) return;
    this->sstable_writer.write(this->immutable_pool.front());

    if (!erase) return;
    this->immutable_pool.erase(this->immutable_pool.begin());
}
void SSTableManager::write_all(bool erase)
{
    if (erase)
    {
        while (!this->immutable_pool.empty())
        {
            this->write_latest(true);
        }
    }
    else
    {
        for (auto& sstable : this->immutable_pool)
            this->sstable_writer.write(sstable);
    }
}


/// loading manager, loader;


std::optional<SSTable> SSTableLoader::load(const std::filesystem::path& path, Arena& arena)
{
    SSTable result(path);
    std::uint64_t offset = 0ull;

    std::unique_ptr<ReadableFile> file = open_readable_file(path);

    if (!file)
        return std::nullopt;

    auto file_header_opt = FileHeaderSection::load(*file, offset);
    if (!file_header_opt) return std::nullopt;
    auto file_header_section = std::move(*file_header_opt);

    auto file_footer_opt = FileFooterSection::load(*file, offset, FileFooterSection::disk_size());
    if (!file_footer_opt) return std::nullopt;
    auto file_footer_section = std::move(*file_footer_opt);

    auto data_opt = DataSectionView::load(*file, offset, file_footer_section.data_offset, file_footer_section.data_block_count);
    if (!data_opt) return std::nullopt;
    auto data_section_view = std::move(*data_opt);

    auto index_opt = IndexSection::load(*file, offset, arena, file_footer_section.index_offset);
    if (!index_opt) return std::nullopt;
    auto index_section = std::move(*index_opt);

    auto bloom_opt = BloomSection::load(*file, offset, file_footer_section.bloom_offset);
    if (!bloom_opt) return std::nullopt;
    auto bloom_section = std::move(*bloom_opt);

    auto meta_opt = MetaSection::load(*file, offset, index_section, arena, file_footer_section.meta_offset);
    if (!meta_opt) return std::nullopt;
    auto meta_section = std::move(*meta_opt);

    result.file_header_section = std::move(file_header_section);
    result.data_section_view = std::move(data_section_view);
    result.index_section = std::move(index_section);
    result.bloom_section = std::move(bloom_section);
    result.meta_section = std::move(meta_section);
    result.file_footer_section = std::move(file_footer_section);

    return result;
}
void SSTableManager::load(Arena& arena, const std::filesystem::path& root_path)
{
    //while (true)
    //{
    //    std::filesystem::path next_name = std::move(SSTableManager::make_table_path(root_path, table_id));
    //    if (next_name.empty()) break;

    //    auto sstable = this->sstable_loader.load(next_name, arena);
    //    if (!sstable) continue;

    //    this->immutable_pool.emplace_back(std::move(*sstable));
    //}
    for (std::uint32_t table_id = 1; ; table_id++)
    {
        auto path = make_table_path(root_path, table_id);
        
        if (!std::filesystem::exists(path))
            break;
    
        auto sstable = sstable_loader.load(path, arena);
        
        if (!sstable)
            break;
    
        immutable_pool.emplace_back(std::move(*sstable));
    }
}



/// DataSection 


void DataSection::DataBlock::add_payload(Payload& payload)
{
    header.payload_disk_size += payload.disk_size();

    // CRC of logical serialized content
    crc32_add_pod<std::uint32_t>(header.crc32, payload.key_size);
    crc32_add_pod<std::uint32_t>(header.crc32, payload.value_size);
    crc32_add_pod<std::uint8_t>(header.crc32, static_cast<std::uint8_t>(payload.type));
    crc32_add_pod<std::uint32_t>(header.crc32, payload.flags);
    crc32_add_pod<std::uint32_t>(header.crc32, payload.reserved);
    crc32_add_pod<std::uint64_t>(header.crc32, payload.seq_num);

    if (payload.key_ptr && payload.key_size > 0)
        compute_crc32(header.crc32, payload.key_ptr, payload.key_size);

    if (payload.value_ptr && payload.value_size > 0)
        compute_crc32(header.crc32, payload.value_ptr, payload.value_size);

    payloads.emplace_back(payload);
}
void DataSection::add_payload(const InternalRecord& record)
{
    Payload payload{};
    payload.key_size = static_cast<std::uint32_t>(record.key_entry.size);
    payload.value_size = static_cast<std::uint32_t>(record.value_entry.size);
    payload.type = record.type;
    payload.flags = 0;
    payload.reserved = 0;
    payload.seq_num = record.seq_num;
    payload.key_ptr = record.key_entry.data;
    payload.value_ptr = record.value_entry.data;

    if (payload.disk_size() > BLOCK_SIZE - DataSection::Header::disk_size()) {
        throw std::runtime_error("payload too large for one data block");
    }

    if (data_blocks.empty())
        init_new_block();

    if (!data_blocks.back().can_payload_fit(payload))
        init_new_block();

    data_blocks.back().add_payload(payload);
}

bool DataSection::Header::write(WritableFile& file, std::uint64_t& offset)
{
    if (!align_to_block_boundary(file, offset, BLOCK_SIZE))
        return false;
    assert(offset / BLOCK_SIZE == (offset + this->disk_size() - 1) / BLOCK_SIZE);

    if (!kvdb::blockio::write_u8_t(file, static_cast<std::uint8_t>(this->type), offset, BLOCK_SIZE))
        return false;
    if (!kvdb::blockio::write_u32_t_le(file, this->payload_disk_size, offset, BLOCK_SIZE))
        return false;
    if (!kvdb::blockio::write_u32_t_le(file, this->crc32, offset, BLOCK_SIZE))
        return false;
    return true;
}
bool DataSection::Payload::write(WritableFile& file, std::uint64_t& offset)
{
    assert(file.current_position() == offset);
    assert(offset / BLOCK_SIZE == (offset + this->disk_size() - 1) / BLOCK_SIZE);

    if (this->disk_size() > BLOCK_SIZE - Header::disk_size())
        throw std::runtime_error("DataSection payload too large for a block");

    if (!kvdb::blockio::write_u32_t_le(file, this->key_size, offset, BLOCK_SIZE)) // notice : block alignment handled by write_type_t_le()
        return false;
    if (!kvdb::blockio::write_u32_t_le(file, this->value_size, offset, BLOCK_SIZE))
        return false;
    if (!kvdb::blockio::write_u8_t(file, static_cast<std::uint8_t>(this->type), offset, BLOCK_SIZE))
        return false;
    if (!kvdb::blockio::write_u32_t_le(file, this->flags, offset, BLOCK_SIZE))
        return false;
    if (!kvdb::blockio::write_u32_t_le(file, this->reserved, offset, BLOCK_SIZE))
        return false;
    if (!kvdb::blockio::write_u64_t_le(file, this->seq_num, offset, BLOCK_SIZE))
        return false;


    if (!kvdb::blockio::write_bytes(
        file,
        std::span<std::byte>(reinterpret_cast<std::byte*>(this->key_ptr), this->key_size),
        offset,
        BLOCK_SIZE
    ))
        return false;
    if (!kvdb::blockio::write_bytes(
        file,
        std::span<std::byte>(reinterpret_cast<std::byte*>(this->value_ptr), this->value_size),
        offset,
        BLOCK_SIZE
    ))
        return false;

    return true;
}
bool DataSection::DataBlock::write(WritableFile& file, std::uint64_t& offset, IndexSection& index_section) {
    //assert(static_cast<std::uint64_t>(file.tellp()) == offset);
    assert(file.current_position() == offset);

    if (!align_to_block_boundary(file, offset, BLOCK_SIZE))
        return false;// each datablock less or equal to the size of physical block size. it was adjusted during .add;

    std::uint64_t data_block_offset = offset;

    if (!this->header.write(file, offset))
        return false;

    bool first_key_set = false;
    std::uint32_t first_key_size = 0, last_key_size = 0;
    void* first_key_ptr = nullptr;
    void* last_key_ptr = nullptr;

    for (auto& payload : this->payloads)
    {
        if (!payload.write(file, offset))
            return false;

        //std::uint64_t key_offset = offset;

        if (!first_key_set)
        {
            first_key_ptr = payload.key_ptr;
            first_key_size = payload.key_size;
            first_key_set = true;
        }

        last_key_ptr = payload.key_ptr;
        last_key_size = payload.key_size;
    }

    if (!payloads.empty())
        index_section.add_index(data_block_offset, first_key_size, last_key_size, first_key_ptr, last_key_ptr);

    return true;
}
void DataSection::write(WritableFile& file, std::uint64_t& offset, IndexSection& index_section, std::uint64_t& data_offset)
{
    if (!align_to_block_boundary(file, offset, BLOCK_SIZE))
        ensure_atomicity();
    data_offset = offset;
    for (auto& data_block : this->data_blocks)
    {
        if (!data_block.write(file, offset, index_section))
            ensure_atomicity();
    }
}

std::optional<DataSectionView> DataSectionView::load(
    ReadableFile& file,
    std::uint64_t& offset,
    const std::uint64_t& first_data_block_offset,
    std::uint32_t data_block_count
)
{
    if (first_data_block_offset == 0 && data_block_count > 0)
        return std::nullopt;

    if (first_data_block_offset != 0)
    {
        //file.seekg(static_cast<std::streamoff>(first_data_block_offset), std::ios::beg);
        //if (!file)
        //    return std::nullopt;

        if (first_data_block_offset % BLOCK_SIZE != 0)
            return std::nullopt;

        offset = first_data_block_offset;
    }

    DataSectionView result{};

    result.data_blocks.reserve(data_block_count);

    while (data_block_count--)
    {
        auto data_block = DataSectionView::DataBlock::load(file, offset);
        if (!data_block)
            return std::nullopt;

        result.data_blocks.emplace_back(std::move(*data_block));
    }

    return result;
}
std::optional<DataSectionView::Header> DataSectionView::Header::load(ReadableFile& file, std::uint64_t& offset)
{
    if (!align_to_block_boundary(file, offset, BLOCK_SIZE))
        return std::nullopt;

    Header result{};
    result.header_offset = offset;

    uint8_t tmp_type;

    if (!kvdb::blockio::read_u8_t(file, tmp_type, offset, BLOCK_SIZE))
        return std::nullopt;

    result.header.type = static_cast<BlockType>(tmp_type);
    if (result.header.type != BlockType::Data)
        return std::nullopt;

    if (!kvdb::blockio::read_u32_t_le(file, result.header.payload_disk_size, offset, BLOCK_SIZE))
        return std::nullopt; 

    if (!kvdb::blockio::read_u32_t_le(file, result.header.crc32, offset, BLOCK_SIZE))
        return std::nullopt;

    if (result.header.payload_disk_size > BLOCK_SIZE - DataSection::Header::disk_size())
        return std::nullopt;

    result.payload_offset = offset;
    result.next_block_offset = result.header_offset + BLOCK_SIZE;

    if (result.payload_offset + result.header.payload_disk_size > result.next_block_offset)
        return std::nullopt;

    return result;
}
//std::optional<DataSection::Payload> DataSection::Payload::load(
//    ReadableFile* file,
//    std::uint64_t& offset,
//    std::uint32_t* must_be_crc
//)
//{
//    assert(static_cast<std::uint64_t>(file.tellg()) == offset);
//
//    Payload payload{};
//
//    if (!kvdb::blockio::read_u32_t_le(file, payload.key_size, offset, BLOCK_SIZE))
//        return std::nullopt;
//
//    if (!kvdb::blockio::read_u32_t_le(file, payload.value_size, offset, BLOCK_SIZE))
//        return std::nullopt;
//
//    uint8_t tmp_type;
//    if (!kvdb::blockio::read_u8_t(file, tmp_type, offset, BLOCK_SIZE))
//        return std::nullopt;
//    payload.type = static_cast<::Type>(tmp_type);
//
//    if (!kvdb::blockio::read_u32_t_le(file, payload.flags, offset, BLOCK_SIZE))
//        return std::nullopt;
//
//    if (!kvdb::blockio::read_u32_t_le(file, payload.reserved, offset, BLOCK_SIZE))
//        return std::nullopt;
//
//    if (!kvdb::blockio::read_u64_t_le(file, payload.seq_num, offset, BLOCK_SIZE))
//        return std::nullopt;
//
//    const std::uint64_t record_size =
//        sizeof(payload.key_size) +
//        sizeof(payload.value_size) +
//        sizeof(payload.type) +
//        sizeof(payload.flags) +
//        sizeof(payload.reserved) +
//        sizeof(payload.seq_num) +
//        payload.key_size +
//        payload.value_size;
//
//    if (payload.key_size > BLOCK_SIZE)
//        return std::nullopt;
//    if (payload.value_size > BLOCK_SIZE)
//        return std::nullopt;
//
//    std::vector<std::byte> key_buf(payload.key_size);
//    std::vector<std::byte> value_buf(payload.value_size);
//
//    if (!kvdb::blockio::read_bytes(file, key_buf.data(), payload.key_size, offset, BLOCK_SIZE))
//        return std::nullopt;
//
//    if (!kvdb::blockio::read_bytes(file, value_buf.data(), payload.value_size, offset, BLOCK_SIZE))
//        return std::nullopt;
//
//    if (must_be_crc != nullptr)
//    {
//        payload.key_ptr = key_buf.data();
//        payload.value_ptr = value_buf.data();
//        payload.append_crc32(*must_be_crc);
//    }
//
//    payload.key_ptr = nullptr;
//    payload.value_ptr = nullptr; // the acutal data wouldn't be loaded (since it takes too much ram)
//
//    return payload;
//}
std::optional<DataSectionView::DataBlock>
DataSectionView::DataBlock::load(ReadableFile& file, std::uint64_t& offset)
{
    //assert(static_cast<std::uint64_t>(file.tellg()) == offset);

    if (!align_to_block_boundary(file, offset, BLOCK_SIZE))
        return std::nullopt;

    DataSectionView::DataBlock result{};

    auto header_res = DataSectionView::Header::load(file, offset);
    if (!header_res)
        return std::nullopt;

    result.header_view = std::move(*header_res);

    // Lazy loading: do not read payload.
    // Just jump to the next physical data block.
    offset = result.header_view.next_block_offset;

    //file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    //if (!file)
    //    return std::nullopt;

    return result;
}

void DataSection::Payload::append_crc32(std::uint32_t& crc_buffer)
{
    crc32_add_pod<std::uint32_t>(crc_buffer, this->key_size);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->value_size);
    crc32_add_pod<std::uint8_t>(crc_buffer, static_cast<std::uint8_t>(this->type));
    crc32_add_pod<std::uint32_t>(crc_buffer, this->flags);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->reserved);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->seq_num);

    compute_crc32(crc_buffer, this->key_ptr, this->key_size);
    compute_crc32(crc_buffer, this->value_ptr, this->value_size);
}
void DataSection::Payload::calculate_crc32(std::uint32_t& crc_buffer)
{
    crc_buffer = ::crc32(0L, Z_NULL, 0);

    crc32_add_pod<std::uint32_t>(crc_buffer, this->key_size);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->value_size);
    crc32_add_pod<std::uint8_t>(crc_buffer, static_cast<std::uint8_t>(this->type));
    crc32_add_pod<std::uint32_t>(crc_buffer, this->flags);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->reserved);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->seq_num);

    compute_crc32(crc_buffer, this->key_ptr, this->key_size);
    compute_crc32(crc_buffer, this->value_ptr, this->value_size);
}
void DataSection::DataBlock::calculate_crc32(std::uint32_t& crc_buffer)
{
    crc_buffer = ::crc32(0L, Z_NULL, 0);
    for (DataSection::Payload& payload : this->payloads)
    {
        payload.append_crc32(crc_buffer);
    }
}



// FileHeaderSection



void FileHeaderSection::write(WritableFile& file, std::uint64_t& offset)
{
    if (!align_to_block_boundary(file, offset, BLOCK_SIZE))
        ensure_atomicity();
    if (!kvdb::blockio::write_u32_t_le(file, this->magic, offset, BLOCK_SIZE))
        ensure_atomicity();
    if (!kvdb::blockio::write_u32_t_le(file, this->version, offset, BLOCK_SIZE))
        ensure_atomicity();
    if (!kvdb::blockio::write_u32_t_le(file, this->flags, offset, BLOCK_SIZE))
        ensure_atomicity();
    if (!kvdb::blockio::write_u32_t_le(file, this->block_size, offset, BLOCK_SIZE))
        ensure_atomicity();
    if (!kvdb::blockio::write_u32_t_le(file, this->table_id, offset, BLOCK_SIZE))
        ensure_atomicity();
    if (!kvdb::blockio::write_u32_t_le(file, this->crc32, offset, BLOCK_SIZE))
        ensure_atomicity();

    assert(file.current_position() < BLOCK_SIZE);
}

std::optional<FileHeaderSection> FileHeaderSection::load(ReadableFile& file, std::uint64_t& offset)
{
    //assert(file.current_ == offset);

    if (!align_to_block_boundary(file, offset, BLOCK_SIZE))
        return std::nullopt;

    FileHeaderSection result{};
    std::uint64_t file_header_offset = offset;

    if(!kvdb::blockio::read_u32_t_le(file, result.magic, offset, BLOCK_SIZE))
        return std::nullopt;
    if (result.magic != FILE_HEADER_MAGIC) return std::nullopt;

    if (!kvdb::blockio::read_u32_t_le(file, result.version, offset, BLOCK_SIZE))
        return std::nullopt;
    if (result.version != SSTABLE_VERSION) return std::nullopt;

    if (!kvdb::blockio::read_u32_t_le(file, result.flags, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u32_t_le(file, result.block_size, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u32_t_le(file, result.table_id, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u32_t_le(file, result.crc32, offset, BLOCK_SIZE))
        return std::nullopt;

    std::uint32_t must_be_crc32;
    result.calculate_crc32(must_be_crc32);

    if (must_be_crc32 != result.crc32) return std::nullopt;
    if (result.block_size != BLOCK_SIZE) return std::nullopt;
    return result;
}

void FileHeaderSection::calculate_crc32(std::uint32_t& crc_buffer)
{
    crc_buffer = ::crc32(0L, Z_NULL, 0);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->magic);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->version);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->flags);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->block_size);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->table_id);
}

// IndexSection


std::optional<IndexSection::Header> IndexSection::Header::load(ReadableFile& file, uint64_t& offset)
{
    if (!align_to_block_boundary(file, offset, BLOCK_SIZE))
        return std::nullopt;

    Header result{};

    uint8_t tmp_type;
    if (!kvdb::blockio::read_u8_t(file, tmp_type, offset, BLOCK_SIZE))
        return std::nullopt;

    result.type = static_cast<BlockType>(tmp_type);
    if (result.type != BlockType::Index)
        return std::nullopt;

    if (!kvdb::blockio::read_u32_t_le(file, result.payload_size, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u32_t_le(file, result.crc32, offset, BLOCK_SIZE))
        return std::nullopt;

    return result;
}
std::optional<IndexSection::Payload> IndexSection::Payload::load(ReadableFile& file, std::uint64_t& offset, Arena& arena)
{
    if (!ensure_fits_in_block(file, IndexSection::Payload::fixed_disk_size(), offset, BLOCK_SIZE))
        return std::nullopt;

    IndexSection::Payload result{};

    if (!kvdb::blockio::read_u64_t_le(file, result.data_block_offset, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u32_t_le(file, result.first_key_size, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u32_t_le(file, result.last_key_size, offset, BLOCK_SIZE))
        return std::nullopt;

    if (result.first_key_size > BLOCK_SIZE)
        return std::nullopt;
    if (result.last_key_size > BLOCK_SIZE)
        return std::nullopt;

    if (!ensure_fits_in_block(file, result.first_key_size + result.last_key_size, offset, BLOCK_SIZE))
        return std::nullopt;

    void* first_key_ptr = arena.alloc(result.first_key_size, alignof(std::byte)); // Arena arena will be handled and provided in some other place
    void* last_key_ptr = arena.alloc(result.last_key_size, alignof(std::byte));

    if (result.first_key_size > 0 && first_key_ptr == nullptr)
        return std::nullopt;
    if (result.last_key_size > 0 && last_key_ptr == nullptr)
        return std::nullopt;

    if (!kvdb::blockio::read_bytes(file, reinterpret_cast<std::byte*>(first_key_ptr), result.first_key_size, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_bytes(file, reinterpret_cast<std::byte*>(last_key_ptr), result.last_key_size, offset, BLOCK_SIZE))
        return std::nullopt;

    result.first_key_ptr = first_key_ptr;
    result.last_key_ptr = last_key_ptr;

    return result;
}
std::optional<IndexSection> IndexSection::load(
    ReadableFile& file,
    std::uint64_t& offset,
    Arena& arena,
    const std::uint64_t& index_offset
)
{
    //assert(file. == offset);

    if (index_offset % BLOCK_SIZE != 0)
        return std::nullopt;

    offset = index_offset;

    IndexSection result{};
    std::uint64_t index_block_offset = offset;

    auto header_res = IndexSection::Header::load(file, offset);
    if (!header_res)
        return std::nullopt;
    result.header = std::move(*header_res);

    // payload

    std::uint64_t payload_bytes_read = 0;

    while (payload_bytes_read < result.header.payload_size)
    {
        auto payload_res = IndexSection::Payload::load(file, offset, arena);
        if (!payload_res)
            return std::nullopt;

        if (payload_res->disk_size() > result.header.payload_size - payload_bytes_read)
            return std::nullopt;

        payload_bytes_read += payload_res->disk_size();
        result.payloads.emplace_back(std::move(*payload_res));
        
    }

    if (payload_bytes_read != result.header.payload_size)
        return std::nullopt;


    std::uint32_t must_be_crc32;
    result.calculate_crc32(must_be_crc32);

    if (must_be_crc32 != result.header.crc32) return std::nullopt;

    return result;
}

void IndexSection::Payload::calculate_crc32(std::uint32_t& crc_buffer)
{
    crc_buffer = ::crc32(0L, Z_NULL, 0);

    this->append_crc32(crc_buffer);
}
void IndexSection::Payload::append_crc32(std::uint32_t& crc_buffer)
{
    crc32_add_pod<std::uint64_t>(crc_buffer, this->data_block_offset);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->first_key_size);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->last_key_size);

    compute_crc32(crc_buffer, this->first_key_ptr, this->first_key_size);
    compute_crc32(crc_buffer, this->last_key_ptr, this->last_key_size);
}
void IndexSection::calculate_crc32(std::uint32_t& crc_buffer)
{
    crc_buffer = ::crc32(0L, Z_NULL, 0);
    for (IndexSection::Payload& payload : this->payloads)
        payload.append_crc32(crc_buffer);
}

void IndexSection::write(WritableFile& file, std::uint64_t& offset, std::uint64_t& index_offset)
{
    assert(file.current_position() == offset);

    if (!align_to_block_boundary(file, offset, BLOCK_SIZE))
        ensure_atomicity();

    index_offset = offset;

    if (!this->header.write(file, offset))
        ensure_atomicity();

    for (auto& payload : this->payloads)
    {
        if (!payload.write(file, offset))
            ensure_atomicity();
    }
}
bool IndexSection::Header::write(WritableFile& file, std::uint64_t& offset)
{
    assert(file.current_position() == offset);
    if (!align_to_block_boundary(file, offset, BLOCK_SIZE))
        return false;
    assert(offset / BLOCK_SIZE == (offset + this->disk_size() - 1) / BLOCK_SIZE);

    if (!kvdb::blockio::write_u8_t(file, static_cast<std::uint8_t>(this->type), offset, BLOCK_SIZE))
        return false;
    if (!kvdb::blockio::write_u32_t_le(file, this->payload_size, offset, BLOCK_SIZE))
        return false;
    if (!kvdb::blockio::write_u32_t_le(file, this->crc32, offset, BLOCK_SIZE))
        return false;
    return true;
}
bool IndexSection::Payload::write(WritableFile& file, std::uint64_t& offset)
{
    assert(file.current_position() == offset);

    if (!ensure_fits_in_block(file, Payload::fixed_disk_size(), offset, BLOCK_SIZE))
        return false;

    if (!kvdb::blockio::write_u64_t_le(file, this->data_block_offset, offset, BLOCK_SIZE))
        return false;

    if (!kvdb::blockio::write_u32_t_le(file, this->first_key_size, offset, BLOCK_SIZE))
        return false;

    if (!kvdb::blockio::write_u32_t_le(file, this->last_key_size, offset, BLOCK_SIZE))
        return false;

    if (!ensure_fits_in_block(file, first_key_size + last_key_size, offset, BLOCK_SIZE))
        return false;

    if (!kvdb::blockio::write_bytes(
        file,
        std::span<std::byte>(reinterpret_cast<std::byte*>(this->first_key_ptr), this->first_key_size),
        offset,
        BLOCK_SIZE
    ))
        return false;

    if (!kvdb::blockio::write_bytes(
        file,
        std::span<std::byte>(reinterpret_cast<std::byte*>(this->last_key_ptr), this->last_key_size),
        offset,
        BLOCK_SIZE
    ))
        return false;

    return true;
}

void IndexSection::add_index(
    std::uint64_t data_block_offset,
    std::uint32_t first_key_size,
    std::uint32_t last_key_size,
    void* first_key_ptr,
    void* last_key_ptr
)
{
    Payload payload{};
    payload.data_block_offset = data_block_offset;
    payload.first_key_size = first_key_size;
    payload.last_key_size = last_key_size;
    payload.first_key_ptr = first_key_ptr;
    payload.last_key_ptr = last_key_ptr;

    this->payloads.emplace_back(payload);

    this->header.payload_size +=
        sizeof(payload.data_block_offset) +
        sizeof(payload.first_key_size) +
        sizeof(payload.last_key_size) +
        payload.first_key_size +
        payload.last_key_size;

    crc32_add_pod<std::uint64_t>(this->header.crc32, payload.data_block_offset);
    crc32_add_pod<std::uint32_t>(this->header.crc32, payload.first_key_size);
    crc32_add_pod<std::uint32_t>(this->header.crc32, payload.last_key_size);

    if (payload.first_key_ptr && payload.first_key_size > 0)
        compute_crc32(this->header.crc32, payload.first_key_ptr, payload.first_key_size);

    if (payload.last_key_ptr && payload.last_key_size > 0)
        compute_crc32(this->header.crc32, payload.last_key_ptr, payload.last_key_size);
}



// BloomSection

std::optional<BloomSection::Header> BloomSection::Header::load(ReadableFile& file, std::uint64_t& offset)
{
    //assert(static_cast<std::uint64_t>(file.tellg()) == offset);

    Header result{};

    std::uint8_t tmp_type;
    if (!kvdb::blockio::read_u8_t(file, tmp_type, offset, BLOCK_SIZE))
        return std::nullopt;

    result.type = static_cast<BlockType>(tmp_type);
    if (result.type != BlockType::Bloom)
        return std::nullopt;

    if (!kvdb::blockio::read_u32_t_le(file, result.payload_size, offset, BLOCK_SIZE))
        return std::nullopt;
    if (result.payload_size != BloomSection::Payload::disk_size())
        return std::nullopt;;

    if (!kvdb::blockio::read_u32_t_le(file, result.crc32, offset, BLOCK_SIZE))
        return std::nullopt;

    return result;
}
std::optional<BloomSection::Payload> BloomSection::Payload::load(ReadableFile& file, std::uint64_t& offset)
{
    //assert(static_cast<std::uint64_t>(file.tellg()) == offset);

    Payload result{};

    std::uint64_t payload_size = 0;

    if (!kvdb::blockio::read_u64_t_le(file, result.bloom_bits, offset, BLOCK_SIZE))
        return std::nullopt;
    
    if (!kvdb::blockio::read_u32_t_le(file, result.hash_count, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u32_t_le(file, result.key_count, offset, BLOCK_SIZE))
        return std::nullopt;

    result.mask.resize(BLOOM_MASK_BIT_SIZE);

    if (!kvdb::blockio::read_bytes(file, reinterpret_cast<std::byte*>(result.mask.data()), BLOOM_MASK_BIT_SIZE, offset, BLOCK_SIZE))
        return std::nullopt;

    return result;
}
std::optional<BloomSection> BloomSection::load(ReadableFile& file, std::uint64_t& offset, const std::uint64_t& bloom_offset)
{
    //assert(static_cast<std::uint64_t>(file.tellg()) == offset);

    if (bloom_offset % BLOCK_SIZE != 0)
        return std::nullopt;

    offset = bloom_offset;

    BloomSection result{};
    std::uint64_t bloom_block_offset = offset;

    auto header_res = BloomSection::Header::load(file, offset);
    if (!header_res)
        return std::nullopt;
    result.header = std::move(*header_res);

    auto payload_res = BloomSection::Payload::load(file, offset);
    if (!payload_res)
        return std::nullopt;
    result.payload = std::move(*payload_res);

    std::uint32_t must_be_crc32;
    result.calculate_crc32(must_be_crc32);

    if (must_be_crc32 != result.header.crc32) return std::nullopt;
    if (result.payload.hash_count != BLOOM_HASH_COUNT) return std::nullopt;
    if (result.payload.disk_size() != result.header.payload_size) return std::nullopt;
    if (result.payload.bloom_bits != result.payload.mask.size()) return std::nullopt;

    return result;
}

void BloomSection::Payload::calculate_crc32(std::uint32_t& crc_buffer)
{
    crc_buffer = ::crc32(0L, Z_NULL, 0);

    crc32_add_pod<std::uint64_t>(crc_buffer, this->bloom_bits);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->hash_count);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->key_count);

    compute_crc32(crc_buffer, this->mask.data(), this->mask.size());
}
void BloomSection::calculate_crc32(std::uint32_t& crc_buffer)
{
    this->payload.calculate_crc32(crc_buffer);
}

bool BloomSection::Header::write(WritableFile& file, std::uint64_t& offset)
{
    if (!align_to_block_boundary(file, offset, BLOCK_SIZE))
        return false;
    assert(offset / BLOCK_SIZE == (offset + this->disk_size() - 1) / BLOCK_SIZE);

    if (!kvdb::blockio::write_u8_t(file, static_cast<uint8_t>(this->type), offset, BLOCK_SIZE))
        return false;

    if (!kvdb::blockio::write_u32_t_le(file, this->payload_size, offset, BLOCK_SIZE))
        return false;

    if (!kvdb::blockio::write_u32_t_le(file, this->crc32, offset, BLOCK_SIZE))
        return false;

    return true;
}
bool BloomSection::Payload::write(WritableFile& file, std::uint64_t& offset)
{
    std::uint64_t old_offset = offset;
    if (!ensure_fits_in_block(file, Payload::disk_size(), offset, BLOCK_SIZE))
        return false;

    assert(old_offset == offset);
    assert(offset / BLOCK_SIZE == (offset + this->disk_size() - 1) / BLOCK_SIZE);

    if (!kvdb::blockio::write_u64_t_le(file, this->bloom_bits, offset, BLOCK_SIZE))
        return false;
    
    if (!kvdb::blockio::write_u32_t_le(file, this->hash_count, offset, BLOCK_SIZE))
        return false;

    if (!kvdb::blockio::write_u32_t_le(file, this->key_count, offset, BLOCK_SIZE))
        return false;

    if (!kvdb::blockio::write_bytes(
        file,
        std::span<std::byte>(reinterpret_cast<std::byte*>(this->mask.data()), this->mask.size()),
        offset,
        BLOCK_SIZE
    ))
        return false;

    return true;
}
void BloomSection::write(WritableFile& file, std::uint64_t& offset, std::uint64_t& bloom_offset)
{
    assert(file.current_position() == offset);

    if (!align_to_block_boundary(file, offset, BLOCK_SIZE))
        ensure_atomicity();
    bloom_offset = offset;

    if (!this->header.write(file, offset))
        ensure_atomicity(); // temporary place holder again
    if (!this->payload.write(file, offset))
        ensure_atomicity();
}

void BloomSection::add_key(const void* key_ptr, std::uint32_t key_size)
{
    if (!key_ptr || key_size == 0)
        return;

    for (std::uint32_t i = 0; i < payload.hash_count; ++i) {
        std::uint64_t h = bloom_hash_i(key_ptr, key_size, i);
        std::uint64_t bit = h % payload.bloom_bits;
        payload.mask[bit] = 1;
    }

    ++payload.key_count;
}
bool BloomSection::may_contain(const void* key_ptr, std::uint32_t key_size) const
{
    if (!key_ptr || !key_size) return false;

    for (std::size_t i = 0; i < payload.hash_count; i++)
    {
        std::uint64_t h = bloom_hash_i(key_ptr, key_size, i);
        std::uint64_t bit = h % payload.bloom_bits;

        if (!payload.mask[bit])
            return false;
    }

    return true;
}
void BloomSection::recompute_crc32()
{
    header.type = BlockType::Bloom;
    header.payload_size = Payload::disk_size();
    header.crc32 = ::crc32(0L, Z_NULL, 0);

    payload.calculate_crc32(header.crc32);
}
void BloomSection::rebuild(const DataSection& data_section)
{
    payload.bloom_bits = static_cast<std::uint32_t>(payload.mask.size());
    payload.hash_count = BLOOM_HASH_COUNT;
    payload.key_count = 0;
    payload.mask.assign(payload.mask.size(), 0);

    for (const auto& block : data_section.data_blocks)
    {
        for (const auto& data_block_payload : block.payloads)
        {
            add_key(data_block_payload.key_ptr, data_block_payload.key_size);
        }
    }

    recompute_crc32();
}



// MetaSection

bool MetaSection::Header::write(WritableFile& file, std::uint64_t& offset)
{
    assert(file.current_position() == offset);

    if (!align_to_block_boundary(file, offset, BLOCK_SIZE))
        return false;

    assert(offset / BLOCK_SIZE == (offset + this->disk_size() - 1) / BLOCK_SIZE);

    if (!kvdb::blockio::write_u8_t(file, static_cast<std::uint8_t>(this->type), offset, BLOCK_SIZE))
        return false;

    if (!kvdb::blockio::write_u32_t_le(file, this->payload_size, offset, BLOCK_SIZE))
        return false;

    if (!kvdb::blockio::write_u32_t_le(file, this->crc32, offset, BLOCK_SIZE))
        return false;

    return true;
}
bool MetaSection::Payload::write(WritableFile& file, std::uint64_t& offset)
{
    std::uint64_t old_offset;
    old_offset = offset;
    assert(file.current_position() == offset);
    
    if (!ensure_fits_in_block(file, Payload::disk_size(), offset, BLOCK_SIZE))
        return false;

    assert(old_offset == offset);

    if (!kvdb::blockio::write_u64_t_le(file, this->record_count, offset, BLOCK_SIZE))
        return false;

    if (!kvdb::blockio::write_u64_t_le(file, this->tombstone_count, offset, BLOCK_SIZE))
        return false;

    if (!kvdb::blockio::write_u64_t_le(file, this->min_seq_num, offset, BLOCK_SIZE))
        return false;

    if (!kvdb::blockio::write_u64_t_le(file, this->max_seq_num, offset, BLOCK_SIZE))
        return false;
    
    if (!kvdb::blockio::write_u32_t_le(file, this->min_key_size, offset, BLOCK_SIZE))
        return false;

    if (!kvdb::blockio::write_u32_t_le(file, this->max_key_size, offset, BLOCK_SIZE))
        return false;

    if (!kvdb::blockio::write_u64_t_le(file, this->data_block_count, offset, BLOCK_SIZE))
        return false;

    if (!kvdb::blockio::write_u64_t_le(file, this->data_bytes, offset, BLOCK_SIZE))
        return false;

    if (this->min_key_size > 0 && this->min_key_ptr == nullptr)
        return false;

    if (this->max_key_size > 0 && this->max_key_ptr == nullptr)
        return false;

    if (!kvdb::blockio::write_bytes(
        file,
        std::span<std::byte>(reinterpret_cast<std::byte*>(this->min_key_ptr), this->min_key_size),
        offset,
        BLOCK_SIZE
    ))
        return false;

    if (!kvdb::blockio::write_bytes(
        file,
        std::span<std::byte>(reinterpret_cast<std::byte*>(this->max_key_ptr), this->max_key_size),
        offset,
        BLOCK_SIZE
    ))
        return false;

    return true;
}
void MetaSection::write(WritableFile& file, std::uint64_t& offset, std::uint64_t& meta_offset)
{
    assert(file.current_position() == offset);

    if (!align_to_block_boundary(file, offset, BLOCK_SIZE))
        ensure_atomicity();
    meta_offset = offset;

    if (!this->header.write(file, offset))
        ensure_atomicity();
    
    if (!this->payload.write(file, offset))
        ensure_atomicity();
    
}

std::optional<MetaSection::Header> MetaSection::Header::load(
    ReadableFile& file,
    std::uint64_t& offset
)
{
    //assert(static_cast<std::uint64_t>(file.tellg()) == offset);
    if (!align_to_block_boundary(file, offset, BLOCK_SIZE))
        return std::nullopt;

    Header result;

    std::uint8_t tmp_type;
    if (!kvdb::blockio::read_u8_t(file, tmp_type, offset, BLOCK_SIZE))
        return std::nullopt;
    
    result.type = static_cast<BlockType>(tmp_type);
    if (result.type != BlockType::Meta)
        return std::nullopt;

    if (!kvdb::blockio::read_u32_t_le(file, result.payload_size, offset, BLOCK_SIZE))
        return std::nullopt;
    
    if (!kvdb::blockio::read_u32_t_le(file, result.crc32, offset, BLOCK_SIZE))
        return std::nullopt;

    return result;
}
std::optional<MetaSection::Payload> MetaSection::Payload::load(ReadableFile& file, std::uint64_t& offset, Arena& arena, MetaSection::Header& header)
{
    //assert(static_cast<std::uint64_t>(file.tellg()) == offset);

    uint64_t old_offset = offset;

    if (!ensure_fits_in_block(file, header.payload_size, offset, BLOCK_SIZE))
        return std::nullopt;

    assert(old_offset == offset);

    Payload result{};

    if (!kvdb::blockio::read_u64_t_le(file, result.record_count, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u64_t_le(file, result.tombstone_count, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u64_t_le(file, result.min_seq_num, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u64_t_le(file, result.max_seq_num, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u32_t_le(file, result.min_key_size, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u32_t_le(file, result.max_key_size, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u64_t_le(file, result.data_block_count, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u64_t_le(file, result.data_bytes, offset, BLOCK_SIZE))
        return std::nullopt;

    const std::uint64_t expected_size = MetaSection::Payload::fixed_disk_size() + result.min_key_size + result.max_key_size;

    if (expected_size != header.payload_size)
        return std::nullopt;

    if (expected_size > BLOCK_SIZE - MetaSection::Header::disk_size())
        return std::nullopt;

    void* min_key_ptr = arena.alloc(result.min_key_size, alignof(std::byte));
    void* max_key_ptr = arena.alloc(result.max_key_size, alignof(std::byte));
    
    if (result.min_key_size > 0 && min_key_ptr == nullptr)
        return std::nullopt;

    if (result.max_key_size > 0 && max_key_ptr == nullptr)
        return std::nullopt;

    if (!kvdb::blockio::read_bytes(file, reinterpret_cast<std::byte*>(min_key_ptr), result.min_key_size, offset, BLOCK_SIZE))
        return std::nullopt;
    
    if (!kvdb::blockio::read_bytes(file, reinterpret_cast<std::byte*>(max_key_ptr), result.max_key_size, offset, BLOCK_SIZE))
        return std::nullopt;

    if (result.disk_size() != header.payload_size)
        return std::nullopt;

    result.min_key_ptr = min_key_ptr;
    result.max_key_ptr = max_key_ptr;

    return result;
}
std::optional<MetaSection> MetaSection::load(
    ReadableFile& file,
    std::uint64_t& offset,
    IndexSection& index_section,
    Arena& arena,
    const std::uint64_t& meta_offset
) {
    //assert(static_cast<std::uint64_t>(file.tellg()) == offset);

    if (meta_offset % BLOCK_SIZE != 0)
        return std::nullopt;

    offset = meta_offset;

    MetaSection result{};
    std::uint64_t meta_block_offset = offset;

    auto header_res = MetaSection::Header::load(file, offset);
    if (!header_res)
        return std::nullopt;
    result.header = std::move(*header_res);

    auto payload_res = MetaSection::Payload::load(file, offset, arena, result.header);
    if (!payload_res)
        return std::nullopt;
    result.payload = std::move(*payload_res);

    std::uint32_t must_be_crc32;
    result.calculate_crc32(must_be_crc32);

    if (must_be_crc32 != result.header.crc32) return std::nullopt;

    //std::uint32_t must_be_tombstone_count = 0;
    //std::uint32_t must_be_min_key_size = std::numeric_limits<std::uint32_t>::max(), must_be_max_key_size = std::numeric_limits<std::uint32_t>::min();
    //std::uint64_t must_be_min_seq_num = std::numeric_limits<std::uint64_t>::max(), must_be_max_seq_num = std::numeric_limits<std::uint64_t>::min();
    //std::uint64_t must_be_data_bytes = 0;

    //for (const auto& block : data_section.data_blocks)
    //{

    //    for (const auto& payload : block.payloads)
    //    {
    //        must_be_tombstone_count += (payload.type == Type::Tombstone);
    //        must_be_min_key_size = std::min(must_be_min_key_size, payload.key_size);
    //        must_be_max_key_size = std::max(must_be_max_key_size, payload.key_size);
    //        must_be_min_seq_num = std::min(must_be_min_seq_num, payload.seq_num);
    //        must_be_max_seq_num = std::max(must_be_max_seq_num, payload.seq_num);
    //        must_be_data_bytes += payload.disk_size();
    //    }
    //}

    //if (data_section.data_blocks.empty())
    //{
    //    must_be_min_key_size = 0;
    //    must_be_max_key_size = 0;
    //    must_be_min_seq_num = 0;
    //    must_be_max_seq_num = 0;
    //}

    //if (result.payload.tombstone_count != must_be_tombstone_count) return std::nullopt;
    //if (result.payload.min_key_size != must_be_min_key_size) return std::nullopt;
    //if (result.payload.max_key_size != must_be_max_key_size) return std::nullopt;
    //if (result.payload.min_seq_num != must_be_min_seq_num) return std::nullopt;
    //if (result.payload.max_seq_num != must_be_max_seq_num) return std::nullopt;
    //if (result.payload.data_bytes != must_be_data_bytes) return std::nullopt; // will be evaluated later, as we make requests
    if (result.header.payload_size != result.payload.disk_size()) return std::nullopt;
    if (result.payload.data_block_count != index_section.payloads.size()) return std::nullopt;

    return result;
}

void MetaSection::Payload::calculate_crc32(std::uint32_t& crc_buffer)
{
    crc_buffer = ::crc32(0L, Z_NULL, 0);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->record_count);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->tombstone_count);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->min_seq_num);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->max_seq_num);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->min_key_size);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->max_key_size);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->data_block_count);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->data_bytes);
    compute_crc32(crc_buffer, this->min_key_ptr, this->min_key_size);
    compute_crc32(crc_buffer, this->max_key_ptr, this->max_key_size);
}
void MetaSection::calculate_crc32(std::uint32_t& crc_buffer)
{
    this->payload.calculate_crc32(crc_buffer);
}

void MetaSection::rebuild(DataSection& data_section, IndexSection& index_section)
{
    this->payload.record_count = 0;
    this->payload.tombstone_count = 0;
    this->payload.min_seq_num = std::numeric_limits<std::uint64_t>::max();
    this->payload.max_seq_num = 0;
    this->payload.min_key_size = 0;
    this->payload.max_key_size = 0;
    this->payload.data_block_count = 0;
    this->payload.data_bytes = 0;

    this->payload.min_key_ptr = nullptr;
    this->payload.max_key_ptr = nullptr;

    this->payload.data_block_count = data_section.data_blocks.size();

    bool has_key = false;

    for (const auto& block : data_section.data_blocks)
    {
        this->payload.data_bytes += block.disk_size();
        for (const auto& payload : block.payloads)
        {
            ++this->payload.record_count;
            this->payload.tombstone_count += (payload.type == Type::Tombstone);
            this->payload.min_seq_num = std::min(this->payload.min_seq_num, payload.seq_num);
            this->payload.max_seq_num = std::max(this->payload.max_seq_num, payload.seq_num);

            ArenaEntry cur(payload.key_ptr, payload.key_size);

            if (!has_key)
            {
                this->payload.min_seq_num = payload.seq_num;
                this->payload.max_seq_num = payload.seq_num;

                this->payload.min_key_ptr = payload.key_ptr;
                this->payload.max_key_ptr = payload.key_ptr;
                this->payload.min_key_size = payload.key_size;
                this->payload.max_key_size = payload.key_size;

                has_key = true;

                continue;
            }

            ArenaEntry min_key(this->payload.min_key_ptr, this->payload.min_key_size);
            ArenaEntry max_key(this->payload.max_key_ptr, this->payload.max_key_size);

            if (cur < min_key)
            {
                this->payload.min_key_ptr = payload.key_ptr;
                this->payload.min_key_size = payload.key_size;
            }

            if (cur > max_key)
            {
                this->payload.max_key_ptr = payload.key_ptr;
                this->payload.max_key_size = payload.key_size;
            }
        }
    }

    if (!has_key)
    {
        this->payload.min_seq_num = 0;
        this->payload.max_seq_num = 0;
        this->payload.min_key_size = 0;
        this->payload.max_key_size = 0;
    
        this->payload.min_key_ptr = nullptr;
        this->payload.max_key_ptr = nullptr;
    }

    this->header.payload_size = this->payload.disk_size();
    this->calculate_crc32(this->header.crc32);
}



// FileFooterSection


bool FileFooterSection::write(WritableFile& file, std::uint64_t& offset)    
{
    assert(file.current_position() == offset);
    assert(offset % BLOCK_SIZE == 0);

    if (!kvdb::blockio::write_u32_t_le(file, this->magic, offset, BLOCK_SIZE))
        return false;
    if (!kvdb::blockio::write_u32_t_le(file, this->version, offset, BLOCK_SIZE))
        return false;
    if (!kvdb::blockio::write_u32_t_le(file, this->reserved, offset, BLOCK_SIZE))
        return false;

    if (!kvdb::blockio::write_u64_t_le(file, this->data_offset, offset, BLOCK_SIZE))
        return false;
    if (!kvdb::blockio::write_u64_t_le(file, this->data_block_count, offset, BLOCK_SIZE))
        return false;

    if (!kvdb::blockio::write_u64_t_le(file, this->index_offset, offset, BLOCK_SIZE))
        return false;
    if (!kvdb::blockio::write_u32_t_le(file, this->index_size, offset, BLOCK_SIZE))
        return false;

    if (!kvdb::blockio::write_u64_t_le(file, this->bloom_offset, offset, BLOCK_SIZE))
        return false;
    if (!kvdb::blockio::write_u32_t_le(file, this->bloom_size, offset, BLOCK_SIZE))
        return false;

    if (!kvdb::blockio::write_u64_t_le(file, this->meta_offset, offset, BLOCK_SIZE))
        return false;
    if (!kvdb::blockio::write_u32_t_le(file, this->meta_size, offset, BLOCK_SIZE))
        return false;

    if (!kvdb::blockio::write_u64_t_le(file, this->file_size, offset, BLOCK_SIZE))
        return false;
    if (!kvdb::blockio::write_u32_t_le(file, this->footer_crc32, offset, BLOCK_SIZE))
        return false;

    return true;
}

void FileFooterSection::finalize(WritableFile& file, std::uint64_t current_offset)
{
    this->file_size = current_offset + FileFooterSection::disk_size();
    this->calculate_crc32(this->footer_crc32);
}

std::optional<FileFooterSection> FileFooterSection::load(
    ReadableFile& file,
    std::uint64_t& offset,
    std::uint64_t file_footer_backwards_offset
){
    if (file_footer_backwards_offset)
    {
        std::uint64_t file_size;
        if (!file.get_file_size(file_size))
            return std::nullopt;
        if (file_size < file_footer_backwards_offset)
            return std::nullopt;
        offset = file_size - file_footer_backwards_offset;
        //if (dir == std::ios::beg)
        //    file.seekg(file_footer_offset, dir);
        //else
        //    file.seekg(-static_cast<std::streamoff>(FileFooterSection::disk_size()), std::ios::end);
        //if (!file)
        //    return std::nullopt;
        //offset = static_cast<std::uint64_t>(file.tellg());
    }

    if (offset % BLOCK_SIZE != 0)
        return std::nullopt;

    FileFooterSection result{};

    if (!kvdb::blockio::read_u32_t_le(file, result.magic, offset, BLOCK_SIZE))
        return std::nullopt;
    if (result.magic != SSTableEntities::FILE_FOOTER_MAGIC)
        return std::nullopt;

    if (!kvdb::blockio::read_u32_t_le(file, result.version, offset, BLOCK_SIZE))
        return std::nullopt;
    if (result.version != SSTABLE_VERSION)
        return std::nullopt;
    
    if (!kvdb::blockio::read_u32_t_le(file, result.reserved, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u64_t_le(file, result.data_offset, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u64_t_le(file, result.data_block_count, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u64_t_le(file, result.index_offset, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u32_t_le(file, result.index_size, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u64_t_le(file, result.bloom_offset, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u32_t_le(file, result.bloom_size, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u64_t_le(file, result.meta_offset, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u32_t_le(file, result.meta_size, offset, BLOCK_SIZE))
        return std::nullopt;

    if (!kvdb::blockio::read_u64_t_le(file, result.file_size, offset, BLOCK_SIZE))
        return std::nullopt;

    //auto cur = file.tellg();
    //file.seekg(0, std::ios::end);
    std::uint64_t actual_file_size;
    if (!file.get_file_size(actual_file_size))
        return std::nullopt;
    //file.seekg(cur);

    if (result.file_size != static_cast<std::uint64_t>(actual_file_size))
        return std::nullopt;

    if (!kvdb::blockio::read_u32_t_le(file, result.footer_crc32, offset, BLOCK_SIZE))
        return std::nullopt;

    std::uint32_t must_be_footer_crc32;
    result.calculate_crc32(must_be_footer_crc32);

    if (must_be_footer_crc32 != result.footer_crc32)
        return std::nullopt;

    return result;
}

void FileFooterSection::calculate_crc32(std::uint32_t& crc_buffer)
{
    crc_buffer = ::crc32(0L, Z_NULL, 0);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->magic);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->version);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->reserved);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->data_offset);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->data_block_count);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->index_offset);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->index_size);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->bloom_offset);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->bloom_size);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->meta_offset);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->meta_size);
    crc32_add_pod<std::uint64_t>(crc_buffer, this->file_size);
}

void FileFooterSection::rebuild(IndexSection& index_block, std::uint64_t index_offset)
{
    this->index_offset = index_offset;
    this->index_size = static_cast<std::uint32_t>(index_block.disk_size());
}
void FileFooterSection::rebuild(BloomSection& bloom_block, std::uint64_t bloom_offset)
{
    this->bloom_offset = bloom_offset;
    this->bloom_size = static_cast<std::uint32_t>(BloomSection::disk_size());
}
void FileFooterSection::rebuild(MetaSection& meta_section, std::uint64_t meta_offset)
{
    this->meta_offset = meta_offset;
    this->meta_size = static_cast<std::uint32_t>(meta_section.disk_size());
}

std::filesystem::path SSTableManager::make_table_path(
    const std::filesystem::path& dir,
    std::uint32_t table_id
) {
    //table_id++;
    return dir / std::format("table-{:09}.sst", table_id);
}

std::filesystem::path SSTableManager::make_tmp_table_path(
    const std::filesystem::path& dir,
    std::uint32_t table_id
) {
    //table_id++;
    return dir / std::format("table-{:09}.sst.tmp", table_id);
}

bool SSTable::fsync(WritableFile& file_out)
{
    if (!file_out.sync())
        return false;
    if (!file_out.close())
        return false;
    if (!durable_rename(this->path, this->final_path, true))
        return false;
    if (!sync_parent_directory(this->final_path))
        return false;
    return true;
}
