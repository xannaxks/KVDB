#ifdef _WIN32

    #ifndef NOMINMAX
        #define NOMINMAX
    #endif

    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif

#endif

#include "sstable.h"
#include <algorithm>
#include <limits>

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
    data_offset(0),
    data_block_count(0),
    index_offset(0),
    index_size(0),
    bloom_offset(0),
    bloom_size(0),
    meta_offset(0),
    meta_size(0),
    file_size(0),
    footer_crc32(0)
{
    this->calculate_crc32(footer_crc32);
}

SSTable::SSTable( std::filesystem::path& path,  std::filesystem::path& final_path)
    : path(std::move(path)),
    final_path(std::move(final_path)),
    file_header_section(),
    data_section(),
    index_section(),
    bloom_section(),
    meta_section(),
    file_footer_section()
{
}
SSTable::SSTable(std::filesystem::path& path):
	path(std::move(path)),
	final_path(),
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


Status SSTable::write()
{
	Result<std::unique_ptr<WritableFile>> file_out_opt = open_writable_file(this->path);
    if(!file_out_opt.is_ok())
		return std::move(file_out_opt.status);

    file_out = std::move(file_out_opt.value);
    if (!file_out)
        return Status{ StatusCode::OpenFailed, "Failed to open SSTable for writing path=" + path.string() };

    Status write_result;

    std::uint64_t offset = 0;

    write_result = file_header_section.write(*file_out, offset);
    if (!write_result.is_ok())
        return write_result;

    // Data write also fills index_block
    std::uint64_t data_offset = 0;
    write_result = data_section.write(*file_out, offset, index_section, data_offset);
    if (!write_result.is_ok())
        return write_result;

    // Build bloom/meta after data/index are finalized
    bloom_section.rebuild(data_section);
    meta_section.rebuild(data_section, index_section);

    std::uint64_t index_offset = 0;
    std::uint64_t bloom_offset = 0;
    std::uint64_t meta_offset = 0;

    write_result = index_section.write(*file_out, offset, index_offset);
    if (!write_result.is_ok())
        return write_result;

    write_result = bloom_section.write(*file_out, offset, bloom_offset);
    if (!write_result.is_ok())
        return write_result;

    write_result = meta_section.write(*file_out, offset, meta_offset);
    if (!write_result.is_ok())
        return write_result;

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

	Status align_to_block_result = align_to_block_boundary(*file_out, offset, BLOCK_SIZE);

    if (!align_to_block_result.is_ok())
        return std::move(align_to_block_result);

    file_footer_section.finalize(*file_out, offset);

	write_result = file_footer_section.write(*file_out, offset);
    if (!write_result.is_ok())
        return write_result;

    Status fsync_result = this->fsync(*file_out);
    if (!fsync_result.is_ok())
        return fsync_result;

    file_out = nullptr;

    return Status::ok();
}
Status SSTableWriter::write(SSTable& sstable)
{
    return sstable.write();
}
Status SSTableManager::write_latest(bool erase)
{
    if (this->immutable_pool.empty()) return Status::ok();

    Status write_result = this->sstable_writer.write(this->immutable_pool.front());
    if (!write_result.is_ok()) return write_result;

    if (!erase) return Status::ok();

    this->immutable_pool.erase(this->immutable_pool.begin());

    return Status::ok();
}
std::vector<Status> SSTableManager::write_all()
{
    std::vector<Status> result;

    for (auto& sstable : this->immutable_pool)
        result.push_back(this->sstable_writer.write(sstable));

    return result;
}


/// loading manager, loader;

Result<SSTable> SSTableLoader::load( std::filesystem::path& path, Arena& arena)
{
    return SSTable::load(path, arena);
}


Result<SSTable> SSTable::load( std::filesystem::path& path, Arena& arena)
{
    SSTable result(path);
    std::uint64_t offset = 0ull;

	Result<std::unique_ptr<ReadableFile>> readable_file_opt = open_readable_file(path);
	if (!readable_file_opt.is_ok())
		return Result<SSTable>::fail(std::move(readable_file_opt.status));

    std::unique_ptr<ReadableFile> file = std::move(readable_file_opt.value);

    if (!file)
        return Result<SSTable>::fail(
            Status{
                StatusCode::OpenFailed,
                "Failed to open SSTable for reading path=" + path.string()
            }
        );

    auto file_header_opt = FileHeaderSection::load(*file, offset);
    if (!file_header_opt.is_ok()) return Result<SSTable>::fail(std::move(file_header_opt.status));
    auto file_header_section = std::move(file_header_opt.value);

    auto file_footer_opt = FileFooterSection::load(*file, offset, FileFooterSection::disk_size());
    if (!file_footer_opt.is_ok()) return Result<SSTable>::fail(std::move(file_footer_opt.status));
    auto file_footer_section = std::move(file_footer_opt.value);

    auto data_opt = DataSectionView::load(*file, offset, file_footer_section.data_offset, file_footer_section.data_block_count);
    if (!data_opt.is_ok()) return Result<SSTable>::fail(std::move(data_opt.status));
    auto data_section_view = std::move(data_opt.value);

    auto index_opt = IndexSection::load(*file, offset, arena, file_footer_section.index_offset);
    if (!index_opt.is_ok()) return Result<SSTable>::fail(std::move(index_opt.status));
    auto index_section = std::move(index_opt.value);

    auto bloom_opt = BloomSection::load(*file, offset, file_footer_section.bloom_offset);
    if (!bloom_opt.is_ok()) return Result<SSTable>::fail(std::move(bloom_opt.status));
    auto bloom_section = std::move(bloom_opt.value);

    auto meta_opt = MetaSection::load(*file, offset, index_section, arena, file_footer_section.meta_offset);
    if (!meta_opt.is_ok()) return Result<SSTable>::fail(std::move(meta_opt.status));
    auto meta_section = std::move(meta_opt.value);

    result.file_header_section = std::move(file_header_section);
    result.data_section_view = std::move(data_section_view);
    result.index_section = std::move(index_section);
    result.bloom_section = std::move(bloom_section);
    result.meta_section = std::move(meta_section);
    result.file_footer_section = std::move(file_footer_section);

    return Result<SSTable>::ok(std::move(result));
}
std::vector<Status> SSTableManager::load(Arena& arena, const std::filesystem::path& root_path)
{
    std::vector<Status> load_results;

    for (std::uint32_t table_id = 1; ; table_id++)
    {
        auto path = make_table_path(root_path, table_id);
        
        if (!std::filesystem::exists(path))
            break;
    
        auto sstable = sstable_loader.load(path, arena);
        
        if (!sstable.is_ok())
        {
			load_results.push_back(std::move(sstable.status));
            continue;
        }
    
        immutable_pool.emplace_back(std::move(sstable.value));
		load_results.push_back(std::move(sstable.status));
    }

    return load_results;
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
Status DataSection::add_payload(const InternalRecord& record)
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
        return Status{
            StatusCode::InvalidPayloadSize,
            std::format(
                "data block payload exceeds block capacity: payload_size={}, capacity={}",
                payload.disk_size(),
                BLOCK_SIZE - DataSection::Header::disk_size()
            )    
        };
    }

    if (data_blocks.empty())
        init_new_block();

    if (!data_blocks.back().can_payload_fit(payload))
        init_new_block();

    data_blocks.back().add_payload(payload);

    return Status::ok();
}

Status DataSection::Header::write(WritableFile& file, std::uint64_t& offset)
{
    Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);
    if (!align_to_block_result.is_ok())
        return std::move(align_to_block_result);

    assert(offset / BLOCK_SIZE == (offset + this->disk_size() - 1) / BLOCK_SIZE);

    Status write_endian_result;

    write_endian_result = kvdb::blockio::write_u8_t(
        file,
        static_cast<std::uint8_t>(this->type),
        offset,
        BLOCK_SIZE
    );
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(
        file,
        this->payload_disk_size,
        offset,
        BLOCK_SIZE
    );
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(
        file,
        this->crc32,
        offset,
        BLOCK_SIZE
    );
    if (!write_endian_result.is_ok())
        return write_endian_result;

    return Status::ok();
}

Status DataSection::Payload::write(WritableFile& file, std::uint64_t& offset)
{
    Result<std::uint64_t> get_position_result = file.current_position();
	if (!get_position_result.is_ok())
		return std::move(get_position_result.status);
     
    assert(get_position_result.value == offset);
    assert(offset / BLOCK_SIZE == (offset + this->disk_size() - 1) / BLOCK_SIZE);

    if (this->disk_size() > BLOCK_SIZE - Header::disk_size())
        return Status{
            StatusCode::InvalidPayloadSize,
            std::format(
                "data block payload exceeds block capacity during writing: payload_size={}, capacity={}",
                this->disk_size(),
                BLOCK_SIZE - DataSection::Header::disk_size()
            )
        };

    Status write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->key_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok()) // notice : block alignment handled by write_type_t_le()
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->value_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u8_t(file, static_cast<std::uint8_t>(this->type), offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->flags, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->reserved, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->seq_num, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_bytes(
        file,
        std::span<std::byte>(reinterpret_cast<std::byte*>(this->key_ptr), this->key_size),
        offset,
        BLOCK_SIZE
    );
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_bytes(
        file,
        std::span<std::byte>(reinterpret_cast<std::byte*>(this->value_ptr), this->value_size),
        offset,
        BLOCK_SIZE
    );
    if (!write_endian_result.is_ok())
        return write_endian_result;

    return Status::ok();
}

Status DataSection::DataBlock::write(WritableFile& file, std::uint64_t& offset, IndexSection& index_section) {
    //assert(static_cast<std::uint64_t>(file.tellp()) == offset);
	Result<std::uint64_t> get_position_result = file.current_position();
    if(!get_position_result.is_ok())
		return std::move(get_position_result.status);

    assert(get_position_result.value == offset);

    Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);
    if (!align_to_block_result.is_ok())
        return std::move(align_to_block_result);

    // each datablock less or equal to the size of physical block size. it was adjusted during .add;

    Status write_result;
    std::uint64_t data_block_offset = offset;

    write_result = this->header.write(file, offset);
    if (!write_result.is_ok())
        return write_result;

    bool first_key_set = false;
    std::uint32_t first_key_size = 0, last_key_size = 0;
    void* first_key_ptr = nullptr;
    void* last_key_ptr = nullptr;

    for (auto& payload : this->payloads)
    {
        write_result = payload.write(file, offset);
        if (!write_result.is_ok())
            return write_result;

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

    return Status::ok();
}
Status DataSection::write(WritableFile& file, std::uint64_t& offset, IndexSection& index_section, std::uint64_t& data_offset)
{
    Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);
    if (!align_to_block_result.is_ok())
        return std::move(align_to_block_result);

    Status write_result;
    data_offset = offset;
    for (auto& data_block : this->data_blocks)
    {
        write_result = data_block.write(file, offset, index_section);
        if (!write_result.is_ok())
            return write_result;
    }
    return Status::ok();
}

Result<DataSectionView> DataSectionView::load(
    ReadableFile& file,
    std::uint64_t& offset,
    const std::uint64_t& first_data_block_offset,
    std::uint32_t data_block_count
)
{
    if (first_data_block_offset == 0 && data_block_count > 0)
        return Result<DataSectionView>::fail(
            Status{
                StatusCode::InvalidAlignment,
                "First data block offset invalid"
            }
        );

    if (first_data_block_offset != 0)
    {
        if (first_data_block_offset % BLOCK_SIZE != 0)
            return Result<DataSectionView>::fail(
                Status{
                    StatusCode::InvalidAlignment,
                    "First data block offset not aligned to block size"
                }
            );

        offset = first_data_block_offset;
    }

    DataSectionView result{};

    result.data_blocks.reserve(data_block_count);

    while (data_block_count--)
    {
        auto data_block = DataSectionView::DataBlock::load(file, offset);
        if (!data_block.is_ok())
            return Result<DataSectionView>::fail(std::move(data_block.status));

        result.data_blocks.emplace_back(std::move(data_block.value));
    }

    return Result<DataSectionView>::ok(std::move(result));
}
Result<DataSectionView::Header> DataSectionView::Header::load(ReadableFile& file, std::uint64_t& offset)
{
    Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);
    if (!align_to_block_result.is_ok())
        return Result<Header>::fail(std::move(align_to_block_result));

    Header result{};
    result.header_offset = offset;

    uint8_t tmp_type;

    Status read_endian_result;

	read_endian_result = std::move(kvdb::blockio::read_u8_t(file, tmp_type, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<Header>::fail(std::move(read_endian_result));

    result.header.type = static_cast<BlockType>(tmp_type);
    if (result.header.type != BlockType::Data)
        return Result<Header>::fail(
            Status{
                StatusCode::InvalidBlockType,
                "Expected Data block type in DataSection"
            }
        );

    read_endian_result = std::move(kvdb::blockio::read_u32_t_le(file, result.header.payload_disk_size, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
		return Result<Header>::fail(std::move(read_endian_result)); 

    read_endian_result = std::move(kvdb::blockio::read_u32_t_le(file, result.header.crc32, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<Header>::fail(std::move(read_endian_result));

    if (result.header.payload_disk_size > BLOCK_SIZE - DataSection::Header::disk_size())
        return Result<Header>::fail(
            Status{
                StatusCode::InvalidPayloadSize,
                std::format(
                    "data block payload exceeds block capacity during data section view header reading: payload_size={}, capacity={}",
                    result.header.payload_disk_size,
                    BLOCK_SIZE - DataSection::Header::disk_size()
                )
            }
        );

    result.payload_offset = offset;
    result.next_block_offset = result.header_offset + BLOCK_SIZE;

    if (result.payload_offset + result.header.payload_disk_size > result.next_block_offset)
        return Result<Header>::fail(
            Status{
                StatusCode::OffsetOverlap,
                "Current payload overlaps with next block"
            }
        );

    return Result<Header>::ok(std::move(result));
}

Result<DataSectionView::DataBlock>
DataSectionView::DataBlock::load(ReadableFile& file, std::uint64_t& offset)
{
    //assert(static_cast<std::uint64_t>(file.tellg()) == offset);

    Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);
    if (!align_to_block_result.is_ok())
        return Result<DataBlock>::fail(std::move(align_to_block_result));

    DataSectionView::DataBlock result{};

    auto header_res = DataSectionView::Header::load(file, offset);
    if (!header_res.is_ok())
        return Result<DataBlock>::fail(std::move(header_res.status));

    result.header_view = std::move(header_res.value);

    // Lazy loading: do not read payload.
    // Just jump to the next physical data block.
    offset = result.header_view.next_block_offset;

    //file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    //if (!file)
    //    return std::nullopt;

    return Result<DataBlock>::ok(std::move(result));
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



Status FileHeaderSection::write(WritableFile& file, std::uint64_t& offset)
{
	Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);
    if (!align_to_block_result.is_ok())
        return std::move(align_to_block_result);

    Status write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->magic, offset, BLOCK_SIZE);;
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->version, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->flags, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->block_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->table_id, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

	write_endian_result = kvdb::blockio::write_u32_t_le(file, this->crc32, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    Result<std::uint64_t> get_position_result = file.current_position();
    if (!get_position_result.is_ok())
        return std::move(get_position_result.status);

    assert(get_position_result.value < BLOCK_SIZE);

    return Status::ok();
}

Result<FileHeaderSection> FileHeaderSection::load(ReadableFile& file, std::uint64_t& offset)
{
    // assert(file.current_ == offset);
    
    Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);
    if (!align_to_block_result.is_ok())
        return Result<FileHeaderSection>::fail(std::move(align_to_block_result));

    FileHeaderSection result{};
    std::uint64_t file_header_offset = offset;

    if(!kvdb::blockio::read_u32_t_le(file, result.magic, offset, BLOCK_SIZE).is_ok())
        return Result<FileHeaderSection>::fail(
            Status{
                StatusCode::Corruption,
                "Failed to read magic number"
            }
        );
    if (result.magic != FILE_HEADER_MAGIC)
        return Result<FileHeaderSection>::fail(
            Status{
                StatusCode::BadMagic,
                std::format(
                    "Invalid SSTable file header section magic: expected=0x{:08x} actual=0x{:08x}",
                    FILE_HEADER_MAGIC, result.magic
                )
            }
        );

	Status read_endian_result;

	read_endian_result = std::move(kvdb::blockio::read_u32_t_le(file, result.version, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<FileHeaderSection>::fail(std::move(read_endian_result));

    if (result.version != SSTABLE_VERSION)
        return Result<FileHeaderSection>::fail(
            Status{
                StatusCode::UnsupportedVersion,
                std::format("unsupported SSTable version: file_version={}, supported_version={}",
                   result.version, SSTABLE_VERSION)
            }
        );

	read_endian_result = std::move(kvdb::blockio::read_u32_t_le(file, result.flags, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<FileHeaderSection>::fail(std::move(read_endian_result));

    read_endian_result = std::move(kvdb::blockio::read_u32_t_le(file, result.block_size, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<FileHeaderSection>::fail(std::move(read_endian_result));

    read_endian_result = std::move(kvdb::blockio::read_u32_t_le(file, result.table_id, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<FileHeaderSection>::fail(std::move(read_endian_result));

    read_endian_result = std::move(kvdb::blockio::read_u32_t_le(file, result.crc32, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<FileHeaderSection>::fail(std::move(read_endian_result));

    std::uint32_t must_be_crc32;
    result.calculate_crc32(must_be_crc32);

    if (must_be_crc32 != result.crc32)
        return Result<FileHeaderSection>::fail(
            Status{
                StatusCode::ChecksumMismatch,
                std::format("SSTable header CRC mismatch: expected={}, actual={}",
                   must_be_crc32, result.crc32)
            }
        );
    if (result.block_size != BLOCK_SIZE) 
        return Result<FileHeaderSection>::fail(
            Status{
                StatusCode::UnsupportedBlockSize,
                std::format(
                    "unsupported SSTable block size found in file header section during loading: file_block_size={}, expected_block_size={}",
                    result.block_size,
                    BLOCK_SIZE
                )
            }
        );
    return Result<FileHeaderSection>::ok(std::move(result));
}

Status FileHeaderSection::calculate_crc32(std::uint32_t& crc_buffer)
{
    crc_buffer = ::crc32(0L, Z_NULL, 0);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->magic);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->version);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->flags);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->block_size);
    crc32_add_pod<std::uint32_t>(crc_buffer, this->table_id);

    return Status::ok();
}

// IndexSection


Result<IndexSection::Header> IndexSection::Header::load(ReadableFile& file, uint64_t& offset)
{
	Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);

    if (!align_to_block_result.is_ok())
        return Result<IndexSection::Header>::fail(std::move(align_to_block_result));

    Header result{};
    uint8_t tmp_type;
	Status read_endian_result;

	read_endian_result = std::move(kvdb::blockio::read_u8_t(file, tmp_type, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<IndexSection::Header>::fail(std::move(read_endian_result));

    result.type = static_cast<BlockType>(tmp_type);
    if (result.type != BlockType::Index)
        return Result<IndexSection::Header>::fail(
            Status{
                StatusCode::InvalidBlockType,
				"Expected Index block type in IndexSection"
            }
        );

	read_endian_result = std::move(kvdb::blockio::read_u32_t_le(file, result.payload_size, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<IndexSection::Header>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.crc32, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<IndexSection::Header>::fail(std::move(read_endian_result));

    return Result<IndexSection::Header>::ok(std::move(result));
}

Result<IndexSection::Payload> IndexSection::Payload::load(ReadableFile& file, std::uint64_t& offset, Arena& arena)
{
	Status ensure_fits_result = ensure_fits_in_block(file, IndexSection::Payload::fixed_disk_size(), offset, BLOCK_SIZE);

    if (!ensure_fits_result.is_ok())
        return Result<IndexSection::Payload>::fail(std::move(ensure_fits_result));

    IndexSection::Payload result{};

	Status read_endian_result;

	read_endian_result = std::move(kvdb::blockio::read_u64_t_le(file, result.data_block_offset, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    read_endian_result = std::move(kvdb::blockio::read_u32_t_le(file, result.first_key_size, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

	read_endian_result = std::move(kvdb::blockio::read_u32_t_le(file, result.last_key_size, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    if (result.first_key_size > BLOCK_SIZE)
        return Result<Payload>::fail(
            Status{
                StatusCode::ChecksumMismatch,
                "First key size exceeds block size"
            }
        );

    if (result.last_key_size > BLOCK_SIZE)
        return Result<Payload>::fail(
            Status{
                StatusCode::ChecksumMismatch,
                "Last key size exceeds block size"
            }
        );

    ensure_fits_result = ensure_fits_in_block(file, result.first_key_size + result.last_key_size, offset, BLOCK_SIZE);
    if (!ensure_fits_result.is_ok())
        return Result<IndexSection::Payload>::fail(std::move(ensure_fits_result));

    Result<void*> arena_alloc_result;

	arena_alloc_result = arena.alloc(result.first_key_size, alignof(std::byte));
    if (!arena_alloc_result.is_ok())
        return Result<Payload>::fail(std::move(arena_alloc_result.status));
    void* first_key_ptr = arena_alloc_result.value;

	arena_alloc_result = arena.alloc(result.last_key_size, alignof(std::byte));
	if (!arena_alloc_result.is_ok())
        return Result<Payload>::fail(std::move(arena_alloc_result.status));
	void* last_key_ptr = arena_alloc_result.value;

    if (result.first_key_size > 0 && first_key_ptr == nullptr)
        return Result<Payload>::fail(
            Status{
                StatusCode::AllocationFailed,
				"Failed to allocate memory for first key"
            }
        );
    if (result.last_key_size > 0 && last_key_ptr == nullptr)
        return Result<Payload>::fail({
            StatusCode::AllocationFailed,
            "Failed to allocate memory for last key"
        });

	read_endian_result = std::move(
        kvdb::blockio::read_bytes(
            file,
            reinterpret_cast<std::byte*>(first_key_ptr),
            result.first_key_size,
            offset,
            BLOCK_SIZE
        )
    );
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(read_endian_result);

	read_endian_result = std::move(
        kvdb::blockio::read_bytes(
            file,
            reinterpret_cast<std::byte*>(last_key_ptr),
            result.last_key_size,
            offset,
            BLOCK_SIZE
        )
    );
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(read_endian_result);

    result.first_key_ptr = first_key_ptr;
    result.last_key_ptr = last_key_ptr;

    return Result<Payload>::ok(std::move(result));
}
Result<IndexSection> IndexSection::load(
    ReadableFile& file,
    std::uint64_t& offset,
    Arena& arena,
    const std::uint64_t& index_offset
)
{
    //assert(file. == offset);

    if (index_offset % BLOCK_SIZE != 0)
        return Result<IndexSection>::fail(
            Status{
                StatusCode::InvalidAlignment,
                "Index section not alinged to block boundary"
            }
        );

    offset = index_offset;

    IndexSection result{};
    std::uint64_t index_block_offset = offset;

    auto header_res = IndexSection::Header::load(file, offset);
    if (!header_res.is_ok())
        return Result<IndexSection>::fail(
            Status{
                StatusCode::InvalidAlignment,
                "Failed to load index section header"
            }
        );
    result.header = std::move(header_res.value);

    // payload

    std::uint64_t payload_bytes_read = 0;

    while (payload_bytes_read < result.header.payload_size)
    {
        auto payload_res = IndexSection::Payload::load(file, offset, arena);
        if (!payload_res.is_ok())
            return Result<IndexSection>::fail(std::move(payload_res.status));

        if (payload_res.value.disk_size() > result.header.payload_size - payload_bytes_read)
            return Result<IndexSection>::fail(
                Status{
                    StatusCode::InvariantViolation,
                    "Payload size exceeds remaining index section payload size"
                }
            );
        payload_bytes_read += payload_res.value.disk_size();
        result.payloads.emplace_back(std::move(payload_res.value));
        
    }

    if (payload_bytes_read != result.header.payload_size)
        return Result<IndexSection>::fail(
            Status{
                StatusCode::InvariantViolation,
                "Payload size does not match index section payload size"
            }
        );


    std::uint32_t must_be_crc32;
    result.calculate_crc32(must_be_crc32);

    if (must_be_crc32 != result.header.crc32)
        return Result<IndexSection>::fail(
            Status{
                StatusCode::ChecksumMismatch,
                std::format("SSTable header CRC mismatch: expected={}, actual={}",
                           must_be_crc32, result.header.crc32)
            }
        );

    return Result<IndexSection>::ok(std::move(result));
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

Status IndexSection::write(WritableFile& file, std::uint64_t& offset, std::uint64_t& index_offset)
{
	Result<std::uint64_t> get_position_result = file.current_position();
	if (!get_position_result.is_ok())
		return std::move(get_position_result.status);

    assert(get_position_result.value == offset);

	Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);

    if (!align_to_block_result.is_ok())
        return std::move(align_to_block_result);

    Status write_result;
    index_offset = offset;

    write_result = this->header.write(file, offset);
    if (!write_result.is_ok())
        return write_result;

    for (auto& payload : this->payloads)
    {
        write_result = payload.write(file, offset);
        if (!write_result.is_ok())
            return write_result;
    }

    return Status::ok();
}
Status IndexSection::Header::write(WritableFile& file, std::uint64_t& offset)
{
    Result<std::uint64_t> get_position_result = file.current_position();
    if (!get_position_result.is_ok())
        return std::move(get_position_result.status);

    assert(get_position_result.value == offset);

	Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);

    if (!align_to_block_result.is_ok())
        return std::move(align_to_block_result);
    
    assert(offset / BLOCK_SIZE == (offset + this->disk_size() - 1) / BLOCK_SIZE);

    Status write_endian_result;

    write_endian_result = kvdb::blockio::write_u8_t(file, static_cast<std::uint8_t>(this->type), offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->payload_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->crc32, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    return Status::ok();
}
Status IndexSection::Payload::write(WritableFile& file, std::uint64_t& offset)
{
    Result<std::uint64_t> get_position_result = file.current_position();
    if (!get_position_result.is_ok())
        return std::move(get_position_result.status);

    assert(get_position_result.value == offset);

    Status ensure_fits_result = ensure_fits_in_block(file, Payload::fixed_disk_size(), offset, BLOCK_SIZE);
    if (!ensure_fits_result.is_ok())
        return std::move(ensure_fits_result);

    Status write_endian_result;

	write_endian_result = std::move(kvdb::blockio::write_u64_t_le(file, this->data_block_offset, offset, BLOCK_SIZE));
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = std::move(kvdb::blockio::write_u32_t_le(file, this->first_key_size, offset, BLOCK_SIZE));
    if (!write_endian_result.is_ok())
        return write_endian_result; 

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->last_key_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

	ensure_fits_result = ensure_fits_in_block(file, this->first_key_size, offset, BLOCK_SIZE);
    if (!ensure_fits_result.is_ok())
        return std::move(ensure_fits_result);
    
    write_endian_result = kvdb::blockio::write_bytes(
        file,
        std::span<std::byte>(reinterpret_cast<std::byte*>(this->first_key_ptr), this->first_key_size),
        offset,
        BLOCK_SIZE
    );
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_bytes(
        file,
        std::span<std::byte>(reinterpret_cast<std::byte*>(this->last_key_ptr), this->last_key_size),
        offset,
        BLOCK_SIZE
    );
    if (!write_endian_result.is_ok())
        return write_endian_result;

    return Status::ok();
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

Result<BloomSection::Header> BloomSection::Header::load(ReadableFile& file, std::uint64_t& offset)
{
    //assert(static_cast<std::uint64_t>(file.tellg()) == offset);

    Header result{};
    std::uint8_t tmp_type;
	Status read_endian_result;

	read_endian_result = std::move(kvdb::blockio::read_u8_t(file, tmp_type, offset, BLOCK_SIZE));
    if (!read_endian_result.is_ok())
        return Result<Header>::fail(std::move(read_endian_result));

    result.type = static_cast<BlockType>(tmp_type);
    if (result.type != BlockType::Bloom)
        return Result<Header>::fail(Status{
            StatusCode::InvariantViolation,
            "Invalid block type for BloomSection header"
        });

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.payload_size, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Header>::fail(std::move(read_endian_result));

    if (result.payload_size != BloomSection::Payload::disk_size())
        return Result<Header>::fail(Status{
            StatusCode::InvariantViolation,
            "Invalid payload size for BloomSection header"
        });

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.crc32, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Header>::fail(std::move(read_endian_result));

    return Result<Header>::ok(std::move(result));
}
Result<BloomSection::Payload> BloomSection::Payload::load(ReadableFile& file, std::uint64_t& offset)
{
    //assert(static_cast<std::uint64_t>(file.tellg()) == offset);

    Payload result{};
    std::uint64_t payload_size = 0;
    Status read_endian_result;

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.bloom_bits, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.hash_count, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.key_count, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    result.mask.resize(BLOOM_MASK_BIT_SIZE);

    read_endian_result = kvdb::blockio::read_bytes(
        file,
        reinterpret_cast<std::byte*>(result.mask.data()),
        BLOOM_MASK_BIT_SIZE,
        offset,
        BLOCK_SIZE
    );
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    return Result<Payload>::ok(std::move(result));
}
Result<BloomSection> BloomSection::load(ReadableFile& file, std::uint64_t& offset, const std::uint64_t& bloom_offset)
{
    //assert(static_cast<std::uint64_t>(file.tellg()) == offset);

    if (bloom_offset % BLOCK_SIZE != 0)
        return Result<BloomSection>::fail(
            Status{
                StatusCode::InvariantViolation,
                "Bloom section offset is not aligned to block boundary during loading"
            }
        );

    offset = bloom_offset;

    BloomSection result{};
    std::uint64_t bloom_block_offset = offset;

    auto header_res = BloomSection::Header::load(file, offset);
    if (!header_res.is_ok())
        return Result<BloomSection>::fail(std::move(header_res.status));
    result.header = std::move(header_res.value);

    auto payload_res = BloomSection::Payload::load(file, offset);
    if (!payload_res.is_ok())
        return Result<BloomSection>::fail(std::move(payload_res.status));
    result.payload = std::move(payload_res.value);

    std::uint32_t must_be_crc32;
    result.calculate_crc32(must_be_crc32);

    if (must_be_crc32 != result.header.crc32)
        return Result<BloomSection>::fail(
            Status{
                StatusCode::ChecksumMismatch,
                std::format("SSTable header CRC mismatch: expected={}, actual={}",
                           must_be_crc32, result.header.crc32)
            }
        );
    if (result.payload.hash_count != BLOOM_HASH_COUNT) return Result<BloomSection>::fail(
        Status{ StatusCode::InvariantViolation, "Bloom section hash count mismatch" });
    if (result.payload.disk_size() != result.header.payload_size) return Result<BloomSection>::fail(
        Status{ StatusCode::InvariantViolation, "Bloom section payload size mismatch" });
    if (result.payload.bloom_bits != result.payload.mask.size()) return Result<BloomSection>::fail(
        Status{ StatusCode::InvariantViolation, "Bloom section bloom bits mismatch" });

    return Result<BloomSection>::ok(std::move(result));
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

Status BloomSection::Header::write(WritableFile& file, std::uint64_t& offset)
{
	Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);

    if (!align_to_block_result.is_ok())
        return std::move(align_to_block_result);

    assert(offset / BLOCK_SIZE == (offset + this->disk_size() - 1) / BLOCK_SIZE);

    Status write_endian_result;

    write_endian_result = kvdb::blockio::write_u8_t(file, static_cast<uint8_t>(this->type), offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->payload_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->crc32, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    return Status::ok();
}
Status BloomSection::Payload::write(WritableFile& file, std::uint64_t& offset)
{
    std::uint64_t old_offset = offset;
    Status ensure_fits_result = ensure_fits_in_block(file, Payload::disk_size(), offset, BLOCK_SIZE);
    if (!ensure_fits_result.is_ok())
        return std::move(ensure_fits_result);

    assert(old_offset == offset);
    assert(offset / BLOCK_SIZE == (offset + this->disk_size() - 1) / BLOCK_SIZE);

    Status write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->bloom_bits, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;
    
    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->hash_count, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->key_count, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result; 

    write_endian_result = kvdb::blockio::write_bytes(
        file,
        std::span<std::byte>(reinterpret_cast<std::byte*>(this->mask.data()), this->mask.size()),
        offset,
        BLOCK_SIZE
    );
    if (!write_endian_result.is_ok())
        return write_endian_result;

    return Status::ok();
}
Status BloomSection::write(WritableFile& file, std::uint64_t& offset, std::uint64_t& bloom_offset)
{
    Result<std::uint64_t> get_position_result = file.current_position();
    if (!get_position_result.is_ok())
        return std::move(get_position_result.status);

    assert(get_position_result.value == offset);

    Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);
    if (!align_to_block_result.is_ok())
        return std::move(align_to_block_result);

    Status write_result;
    bloom_offset = offset;

    write_result = this->header.write(file, offset);
    if (!write_result.is_ok())
        return write_result;

    write_result = this->payload.write(file, offset);
    if (!write_result.is_ok())
        return write_result;
    
    return Status::ok();
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

Status MetaSection::Header::write(WritableFile& file, std::uint64_t& offset)
{
    Result<std::uint64_t> get_position_result = file.current_position();
    if (!get_position_result.is_ok())
        return std::move(get_position_result.status);

    assert(get_position_result.value == offset);

	Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);

    if (!align_to_block_result.is_ok())
        return std::move(align_to_block_result);

    assert(offset / BLOCK_SIZE == (offset + this->disk_size() - 1) / BLOCK_SIZE);

    Status write_endian_result;

    write_endian_result = kvdb::blockio::write_u8_t(file, static_cast<std::uint8_t>(this->type), offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->payload_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->crc32, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    return Status::ok();
}
Status MetaSection::Payload::write(WritableFile& file, std::uint64_t& offset)
{
    std::uint64_t old_offset;
    old_offset = offset;

    Result<std::uint64_t> get_position_result = file.current_position();
    if (!get_position_result.is_ok())
        return std::move(get_position_result.status);

    assert(get_position_result.value == offset);

	Status ensure_fits_result = ensure_fits_in_block(file, Payload::disk_size(), offset, BLOCK_SIZE);

    if (!ensure_fits_result.is_ok())
        return std::move(ensure_fits_result);

    assert(old_offset == offset);

	Status write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->record_count, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->tombstone_count, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->min_seq_num, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->max_seq_num, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;
    
    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->min_key_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->max_key_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->data_block_count, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->data_bytes, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    if (this->min_key_size > 0 && this->min_key_ptr == nullptr)
        return Status{
            StatusCode::InvariantViolation,
			"Failed to write meta section payload: min key size is greater than 0 but min key pointer is null"
        };

    if (this->max_key_size > 0 && this->max_key_ptr == nullptr)
        return Status{
            StatusCode::InvariantViolation,
			"Failed to write meta section payload: max key size is greater than 0 but max key pointer is null"
        };

    write_endian_result = kvdb::blockio::write_bytes(
        file,
        std::span<std::byte>(reinterpret_cast<std::byte*>(this->min_key_ptr), this->min_key_size),
        offset,
        BLOCK_SIZE
    );
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_bytes(
        file,
        std::span<std::byte>(reinterpret_cast<std::byte*>(this->max_key_ptr), this->max_key_size),
        offset,
        BLOCK_SIZE
    );
    if (!write_endian_result.is_ok())
        return write_endian_result;

    return Status::ok();
}
Status MetaSection::write(WritableFile& file, std::uint64_t& offset, std::uint64_t& meta_offset)
{
    Result<std::uint64_t> get_position_result = file.current_position();
    if (!get_position_result.is_ok())
        return std::move(get_position_result.status);

    assert(get_position_result.value == offset);

	Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);

    if (!align_to_block_result.is_ok())
        return std::move(align_to_block_result);

    Status write_result;
    meta_offset = offset;

    write_result = this->header.write(file, offset);
    if (!write_result.is_ok())
        return write_result;
    
    write_result = this->payload.write(file, offset);
    if (!write_result.is_ok())
        return write_result;
    
}

Result<MetaSection::Header> MetaSection::Header::load(
    ReadableFile& file,
    std::uint64_t& offset
)
{
    //assert(static_cast<std::uint64_t>(file.tellg()) == offset);
	Status align_to_block_result = align_to_block_boundary(file, offset, BLOCK_SIZE);

    if (!align_to_block_result.is_ok())
        return Result<MetaSection::Header>::fail(std::move(align_to_block_result));

    Header result;
    std::uint8_t tmp_type;
	Status read_endian_result;

    read_endian_result = kvdb::blockio::read_u8_t(file, tmp_type, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Header>::fail(std::move(read_endian_result));
    
    result.type = static_cast<BlockType>(tmp_type);
    if (result.type != BlockType::Meta)
        return Result<Header>::fail(
            Status{
                StatusCode::InvalidBlockType,
				"Expected Meta block type in MetaSection"
            }
        );

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.payload_size, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Header>::fail(std::move(read_endian_result));
    
    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.crc32, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Header>::fail(std::move(read_endian_result));

    return Result<Header>::ok(std::move(result));
}
Result<MetaSection::Payload> MetaSection::Payload::load(ReadableFile& file, std::uint64_t& offset, Arena& arena, MetaSection::Header& header)
{
    //assert(static_cast<std::uint64_t>(file.tellg()) == offset);

    uint64_t old_offset = offset;

	Status ensure_fits_result = ensure_fits_in_block(file, header.payload_size, offset, BLOCK_SIZE);

    if (!ensure_fits_result.is_ok())
        return Result<Payload>::fail(std::move(ensure_fits_result));

    assert(old_offset == offset);

    Payload result{};
	Status read_endian_result;

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.record_count, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.tombstone_count, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.min_seq_num, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.max_seq_num, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.min_key_size, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.max_key_size, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.data_block_count, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.data_bytes, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    const std::uint64_t expected_size = MetaSection::Payload::fixed_disk_size() + result.min_key_size + result.max_key_size;

    if (expected_size != header.payload_size)
        return Result<Payload>::fail(
            Status{
                StatusCode::InvariantViolation,
				"Meta section payload size mismatch during loading"
            }
        );

    if (expected_size > BLOCK_SIZE - MetaSection::Header::disk_size())
        return Result<Payload>::fail(
            Status{
                StatusCode::InvariantViolation,
				"Meta section payload size exceeds block size"
            }
        );

    Result<void*> arena_alloc_result;

	arena_alloc_result = arena.alloc(result.min_key_size, alignof(std::byte));
    if (!arena_alloc_result.is_ok())
        return Result<Payload>::fail(std::move(arena_alloc_result.status));
    void* min_key_ptr = arena_alloc_result.value;

	arena_alloc_result = arena.alloc(result.max_key_size, alignof(std::byte));
    if (!arena_alloc_result.is_ok())
        return Result<Payload>::fail(std::move(arena_alloc_result.status));
    void* max_key_ptr = arena_alloc_result.value;
    
    if (result.min_key_size > 0 && min_key_ptr == nullptr)
        return Result<Payload>::fail(
            Status{
                StatusCode::AllocationFailed,
				"Failed to allocate memory for min key"
            }
        );

    if (result.max_key_size > 0 && max_key_ptr == nullptr)
        return Result<Payload>::fail(
            Status{
                StatusCode::AllocationFailed,
				"Failed to allocate memory for max key"
            }
        );

    read_endian_result = kvdb::blockio::read_bytes(
        file,
        reinterpret_cast<std::byte*>(min_key_ptr),
        result.min_key_size,
        offset,
        BLOCK_SIZE
    );
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));
    
    read_endian_result = kvdb::blockio::read_bytes(
        file,
        reinterpret_cast<std::byte*>(max_key_ptr),
        result.max_key_size,
        offset,
        BLOCK_SIZE
    );
    if (!read_endian_result.is_ok())
        return Result<Payload>::fail(std::move(read_endian_result));

    if (result.disk_size() != header.payload_size)
        return Result<Payload>::fail(
            Status{
                StatusCode::InvariantViolation,
				"Meta section payload size mismatch during loading"
            }
        );

    result.min_key_ptr = min_key_ptr;
    result.max_key_ptr = max_key_ptr;

    return Result<Payload>::ok(std::move(result));
}
Result<MetaSection> MetaSection::load(
    ReadableFile& file,
    std::uint64_t& offset,
    IndexSection& index_section,
    Arena& arena,
    const std::uint64_t& meta_offset
) {
    //assert(static_cast<std::uint64_t>(file.tellg()) == offset);

    if (meta_offset % BLOCK_SIZE != 0)
        return Result<MetaSection>::fail(
            Status{
                StatusCode::InvalidBlockAlignment,
				"Meta section offset is not aligned to block size"
            }
        );

    offset = meta_offset;

    MetaSection result{};
    std::uint64_t meta_block_offset = offset;

    auto header_res = MetaSection::Header::load(file, offset);
    if (!header_res.is_ok())
        return Result<MetaSection>::fail(std::move(header_res.status));
    result.header = std::move(header_res.value);

    auto payload_res = MetaSection::Payload::load(file, offset, arena, result.header);
    if (!payload_res.is_ok())
        return Result<MetaSection>::fail(std::move(payload_res.status));
    result.payload = std::move(payload_res.value);

    std::uint32_t must_be_crc32;
    result.calculate_crc32(must_be_crc32);

    if (must_be_crc32 != result.header.crc32)
        return Result<MetaSection>::fail(
            Status{
                StatusCode::ChecksumMismatch,
                std::format("SSTable header CRC mismatch: expected={}, actual={}",
                   must_be_crc32, result.header.crc32)
            }
        );

    if (result.header.payload_size != result.payload.disk_size())
        return Result<MetaSection>::fail(
            Status{
                StatusCode::InvariantViolation,
                "Meta section header payload size does not match actual payload size during loading"
            }
		);  
    if (result.payload.data_block_count != index_section.payloads.size()) 
        return Result<MetaSection>::fail(
            Status{
                StatusCode::InvariantViolation,
                "Meta section data block count does not match index section data block count during loading"
            }
		);

    return Result<MetaSection>::ok(std::move(result));
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


Status FileFooterSection::write(WritableFile& file, std::uint64_t& offset)    
{
    Result<std::uint64_t> get_position_result = file.current_position();
    if (!get_position_result.is_ok())
        return std::move(get_position_result.status);

    assert(get_position_result.value == offset);
    assert(offset % BLOCK_SIZE == 0);

    Status write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->magic, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->version, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;
    
    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->reserved, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->data_offset, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->data_block_count, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->index_offset, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->index_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->bloom_offset, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->bloom_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->meta_offset, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->meta_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u64_t_le(file, this->file_size, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    write_endian_result = kvdb::blockio::write_u32_t_le(file, this->footer_crc32, offset, BLOCK_SIZE);
    if (!write_endian_result.is_ok())
        return write_endian_result;

    return Status::ok();
}

void FileFooterSection::finalize(WritableFile& file, std::uint64_t current_offset)
{
    this->file_size = current_offset + FileFooterSection::disk_size();
    this->calculate_crc32(this->footer_crc32);
}

Result<FileFooterSection> FileFooterSection::load(
    ReadableFile& file,
    std::uint64_t& offset,
    std::uint64_t file_footer_backwards_offset
){
    if (file_footer_backwards_offset)
    {
        std::uint64_t file_size;

		Status get_file_size_result = file.get_file_size(file_size);

        if (!get_file_size_result.is_ok() or file_size == 0)
            return Result<FileFooterSection>::fail(std::move(get_file_size_result));

        if (file_size < file_footer_backwards_offset)
            return Result<FileFooterSection>::fail(
                Status{
                    StatusCode::InvariantViolation,
					"File footer backwards offset is greater than file size during file footer loading"
                }
            );

        offset = file_size - file_footer_backwards_offset;
    }

    if (offset % BLOCK_SIZE != 0)
        return Result<FileFooterSection>::fail(
            Status{
                StatusCode::InvariantViolation,
				"File footer offset is not aligned to block size during file footer loading"
            }
        );

    FileFooterSection result{};
    Status read_endian_result;

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.magic, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    if (result.magic != SSTableEntities::FILE_FOOTER_MAGIC)
        return Result<FileFooterSection>::fail(
            Status{
                StatusCode::BadMagic,
                std::format(
                    "File footer magic number is invalid during file footer loading: expected=0x{:08x} actual=0x{:08x}",
                    FILE_FOOTER_MAGIC,
                    result.magic
                )
            }
        );

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.version, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    if (result.version != SSTABLE_VERSION)
        return Result<FileFooterSection>::fail(
            Status{
                StatusCode::UnsupportedVersion,
                std::format("SSTable version is not relevant during sstable file footer section load: file_version={}, supported_version={}",
                    result.version, SSTABLE_VERSION)
            }
        );
    
    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.reserved, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.data_offset, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.data_block_count, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.index_offset, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.index_size, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.bloom_offset, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.bloom_size, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.meta_offset, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.meta_size, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    read_endian_result = kvdb::blockio::read_u64_t_le(file, result.file_size, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    std::uint64_t actual_file_size;

	Status get_file_size_result = file.get_file_size(actual_file_size);

    if (!get_file_size_result.is_ok() || actual_file_size == 0)
        return Result<FileFooterSection>::fail(std::move(get_file_size_result));

    if (result.file_size != static_cast<std::uint64_t>(actual_file_size))
        return Result<FileFooterSection>::fail(
            Status{
                StatusCode::InvariantViolation,
                "Sizes of file doesn't coresponds, actual one with found in file footer"
            }
        );

    read_endian_result = kvdb::blockio::read_u32_t_le(file, result.footer_crc32, offset, BLOCK_SIZE);
    if (!read_endian_result.is_ok())
        return Result<FileFooterSection>::fail(std::move(read_endian_result));

    std::uint32_t must_be_footer_crc32;
    result.calculate_crc32(must_be_footer_crc32);

    if (must_be_footer_crc32 != result.footer_crc32)
        return Result<FileFooterSection>::fail(
            Status{
                StatusCode::ChecksumMismatch,
                std::format("SSTable footer CRC mismatch: expected={}, actual={}",
                   must_be_footer_crc32, result.footer_crc32)
            }
        );

    return Result<FileFooterSection>::ok(std::move(result));
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

Status SSTable::fsync(WritableFile& file_out)
{
	Status results = file_out.sync();

    if (!results.is_ok())
        return results;

    results = file_out.close();

    if (!results.is_ok())
        return results;

    results = file_out.durable_rename(this->final_path, true);

    if (!results.is_ok())
        return results;

    results = file_out.sync_parent_directory();

    if (!results.is_ok())
        return results;

    return Status::ok();
}


// Iterator implementation

SSTableIterator::SSTableIterator(
    const SSTable& sstable,
    std::unique_ptr<ReadableFile>&& file,
    Arena& arena
) :
    sstable(sstable),
    file(std::move(file)),
    arena(arena),
    current_offset(sstable.file_footer_section.data_offset),
    data_block_count(sstable.file_footer_section.data_block_count),
    next_block_index(0),
    record_index(0),
    valid_(false),
    status_(std::move(Status::ok()))
{
}

Status SSTableIterator::next()
{
    if (!valid_)
        return Status::ok();

    ++record_index;

    if (record_index < current_block_records.size())
        return Status::ok();

    status_ = load_next_block();
    return status_;
}

bool SSTableIterator::valid() const
{
    return this->valid_;
}
Status SSTableIterator::status() const
{
    return this->status_;
}

const InternalRecord& SSTableIterator::record() const
{
    assert(valid_);
    assert(record_index < current_block_records.size());

    return current_block_records[record_index];
}

Status SSTableIterator::load_next_block()
{
    this->record_index = 0;
    valid_ = false;
    this->current_block_records.clear();

    if (this->next_block_index >= data_block_count)
        return Status::ok();

    const std::uint64_t block_start = this->current_offset;

    if (block_start % BLOCK_SIZE != 0) {
        return Status{
            StatusCode::InvalidBlockAlignment,
            "Data block offset is not aligned"
        };
    }

    const std::uint64_t block_end = block_start + BLOCK_SIZE;

    DataSection::DataBlock data_block{};
    Status read_result;

    std::uint8_t block_type = 0;

    read_result = kvdb::blockio::read_u8_t(
        *this->file,
        block_type,
        this->current_offset,
        BLOCK_SIZE
    );
    if (!read_result.is_ok())
        return read_result;

    if (static_cast<BlockType>(block_type) != BlockType::Data) {
        return Status{
            StatusCode::InvalidBlockType,
            "Block type mismatch during reading at offset " +
                std::to_string(block_start)
        };
    }

    data_block.header.type = static_cast<BlockType>(block_type);

    read_result = kvdb::blockio::read_u32_t_le(
        *this->file,
        data_block.header.payload_disk_size,
        this->current_offset,
        BLOCK_SIZE
    );
    if (!read_result.is_ok())
        return read_result;

    std::uint32_t expected_crc = 0;

    read_result = kvdb::blockio::read_u32_t_le(
        *this->file,
        expected_crc,
        this->current_offset,
        BLOCK_SIZE
    );
    if (!read_result.is_ok())
        return read_result;

    if (data_block.header.payload_disk_size >
        BLOCK_SIZE - DataSection::Header::disk_size())
    {
        return Status{
            StatusCode::InvalidPayloadSize,
            "Data block payload size exceeds block capacity"
        };
    }

    const std::uint64_t payload_start = this->current_offset;
    const std::uint64_t payload_end =
        payload_start + data_block.header.payload_disk_size;

    if (payload_end > block_end) {
        return Status{
            StatusCode::OffsetOverlap,
            "Data block payload overlaps block boundary"
        };
    }

    while (this->current_offset < payload_end)
    {
        if (payload_end - this->current_offset < DataSection::Payload::fixed_part_disk_size()) {
            return Status{
                StatusCode::Corruption,
                "Not enough bytes for payload fixed part"
            };
        }

        DataSection::Payload payload{};

        const std::uint64_t payload_record_start = this->current_offset;

        read_result = kvdb::blockio::read_u32_t_le(
            *this->file,
            payload.key_size,
            this->current_offset,
            BLOCK_SIZE
        );
        if (!read_result.is_ok())
            return read_result;

        read_result = kvdb::blockio::read_u32_t_le(
            *this->file,
            payload.value_size,
            this->current_offset,
            BLOCK_SIZE
        );
        if (!read_result.is_ok())
            return read_result;

        std::uint8_t type = 0;

        read_result = kvdb::blockio::read_u8_t(
            *this->file,
            type,
            this->current_offset,
            BLOCK_SIZE
        );
        if (!read_result.is_ok())
            return read_result;

        payload.type = static_cast<::Type>(type);

        read_result = kvdb::blockio::read_u32_t_le(
            *this->file,
            payload.flags,
            this->current_offset,
            BLOCK_SIZE
        );
        if (!read_result.is_ok())
            return read_result;

        read_result = kvdb::blockio::read_u32_t_le(
            *this->file,
            payload.reserved,
            this->current_offset,
            BLOCK_SIZE
        );
        if (!read_result.is_ok())
            return read_result;

        read_result = kvdb::blockio::read_u64_t_le(
            *this->file,
            payload.seq_num,
            this->current_offset,
            BLOCK_SIZE
        );
        if (!read_result.is_ok())
            return read_result;

        const std::uint64_t remaining_payload =
            payload_end - this->current_offset;

        if (payload.key_size > remaining_payload ||
            payload.value_size > remaining_payload - payload.key_size)
        {
            return Status{
                StatusCode::Corruption,
                std::format(
                    "Invalid payload sizes: key_size={}, value_size={}, record_offset={}",
                    payload.key_size,
                    payload.value_size,
                    payload_record_start
                )
            };
        }

        auto key_alloc = this->arena.alloc(payload.key_size, alignof(std::byte));
        if (!key_alloc.is_ok())
            return std::move(key_alloc.status);

        auto value_alloc = this->arena.alloc(payload.value_size, alignof(std::byte));
        if (!value_alloc.is_ok())
            return std::move(value_alloc.status);

        payload.key_ptr = key_alloc.value;
        payload.value_ptr = value_alloc.value;

        read_result = kvdb::blockio::read_bytes(
            *this->file,
            reinterpret_cast<std::byte*>(payload.key_ptr),
            payload.key_size,
            this->current_offset,
            BLOCK_SIZE
        );
        if (!read_result.is_ok())
            return read_result;

        read_result = kvdb::blockio::read_bytes(
            *this->file,
            reinterpret_cast<std::byte*>(payload.value_ptr),
            payload.value_size,
            this->current_offset,
            BLOCK_SIZE
        );
        if (!read_result.is_ok())
            return read_result;

        data_block.payloads.emplace_back(std::move(payload));
    }

    if (this->current_offset != payload_end) {
        return Status{
            StatusCode::Corruption,
            "Payload parser did not stop exactly at payload_end"
        };
    }

    std::uint32_t actual_crc = 0;
    data_block.calculate_crc32(actual_crc);

    if (actual_crc != expected_crc) {
        return Status{
            StatusCode::ChecksumMismatch,
            std::format(
                "Data block CRC mismatch: expected={}, actual={}",
                expected_crc,
                actual_crc
            )
        };
    }

    for (auto& payload : data_block.payloads)
    {
        InternalRecord record{};
        record.key_entry = ArenaEntry(payload.key_ptr, payload.key_size);
        record.value_entry = ArenaEntry(payload.value_ptr, payload.value_size);

        record.type = payload.type;
        record.seq_num = payload.seq_num;

        this->current_block_records.emplace_back(record);
    }

    this->current_offset = block_end;
    next_block_index++; 

    if (current_block_records.empty())
    {
        return Status
        {
            StatusCode::Corruption,
            "Loaded empty data block"
        };
    }

    valid_ = true;
    return Status::ok();
}

Status SSTableIterator::seek_to_first()
{
    current_offset = sstable.file_footer_section.data_offset;
    data_block_count = sstable.file_footer_section.data_block_count;
    next_block_index = 0;
    record_index = 0;
    valid_ = false;
    status_ = Status::ok();
    current_block_records.clear();

    status_ = load_next_block();
    return status_;
}

// sstable getters
const SSTableEntities::FileHeaderSection& SSTable::get_file_header_section() const
{
    return this->file_header_section;
}
const SSTableEntities::DataSection& SSTable::get_data_section() const
{
    return this->data_section;
}
const SSTableEntities::DataSectionView& SSTable::get_data_section_view() const
{
    return this->data_section_view;
}
const SSTableEntities::IndexSection& SSTable::get_index_section() const
{
    return this->index_section;
}
const SSTableEntities::BloomSection& SSTable::get_bloom_section() const
{
    return this->bloom_section;
}
const SSTableEntities::MetaSection& SSTable::get_meta_section() const
{
    return this->meta_section;
}
const SSTableEntities::FileFooterSection& SSTable::get_file_footer_section() const
{
    return this->file_footer_section;
}
const std::filesystem::path& SSTable::get_path() const
{
    return this->path;
}
const std::filesystem::path& SSTable::get_final_path() const
{
    return this->final_path;
}