#include "sstable.h"
#include "arena.h"
#include <cassert>

using namespace SSTableEntities;

// bloom hashes

static uint64_t fnv1a64(const void* data, uint32_t size, uint64_t seed)
{
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = 14695981039346656037ull ^ seed;

    for (uint32_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }

    return h;
}

static uint64_t bloom_hash_i(const void* key, uint32_t key_size, uint32_t i)
{
    uint64_t h1 = fnv1a64(key, key_size, 0x9E3779B97F4A7C15ull);
    uint64_t h2 = fnv1a64(key, key_size, 0xC2B2AE3D27D4EB4Full);

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
        sizeof(mask)
        );
}

std::size_t MetaSection::disk_size()
{
    return Header::disk_size() + Payload::disk_size();
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
        sizeof(data_bytes)
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
        sizeof(footer_crc32)
        );
}

/// helpers

inline void compute_crc32(uint32_t& crc, const void* ptr, std::size_t size)
{
    crc = ::crc32(crc, reinterpret_cast<const Bytef*>(ptr), static_cast<uInt>(size));
}
inline uint32_t crc32_of(const void* ptr, std::size_t size)
{
    uint32_t crc = 0;
    crc = ::crc32(0L, Z_NULL, 0);
    compute_crc32(crc, ptr, size);
    return crc;
}
//template <typename T>
//inline void crc32_add_pod(uint32_t& crc, const T& value)
//{
//    compute_crc32(crc, &value, sizeof(T));
//}

/// constructors

FileHeaderSection::FileHeaderSection(uint32_t table_id)
    : magic(FILE_HEADER_MAGIC),
    version(SSTABLE_VERSION),
    flags(0),
    block_size(BLOCK_SIZE),
    table_id(table_id),
    crc32(0)
{
    crc32 = ::crc32(0L, Z_NULL, 0);
    crc32_add_pod(crc32, magic);
    crc32_add_pod(crc32, version);
    crc32_add_pod(crc32, flags);
    crc32_add_pod(crc32, block_size);
    crc32_add_pod(crc32, this->table_id);
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
    payload.mask.reset();

    crc32_add_pod(header.crc32, payload.bloom_bits);
    crc32_add_pod(header.crc32, payload.hash_count);
    crc32_add_pod(header.crc32, payload.key_count);

    // bitset object representation is implementation-defined,
    // but okay if this is in-memory init only.
    compute_crc32(header.crc32, &payload.mask, sizeof(payload.mask));
}

MetaSection::MetaSection()
{
    header.type = BlockType::Meta;
    header.payload_size = Payload::disk_size();
    header.crc32 = ::crc32(0L, Z_NULL, 0);

    payload.record_count = 0;
    payload.tombstone_count = 0;
    payload.min_seq_num = 0;
    payload.max_seq_num = 0;
    payload.min_key_size = 0;
    payload.max_key_size = 0;
    payload.data_block_count = 0;
    payload.data_bytes = 0;

    crc32_add_pod(header.crc32, payload.record_count);
    crc32_add_pod(header.crc32, payload.tombstone_count);
    crc32_add_pod(header.crc32, payload.min_seq_num);
    crc32_add_pod(header.crc32, payload.max_seq_num);
    crc32_add_pod(header.crc32, payload.min_key_size);
    crc32_add_pod(header.crc32, payload.max_key_size);
    crc32_add_pod(header.crc32, payload.data_block_count);
    crc32_add_pod(header.crc32, payload.data_bytes);
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

    crc32_add_pod(footer_crc32, magic);
    crc32_add_pod(footer_crc32, version);
    crc32_add_pod(footer_crc32, reserved);
    crc32_add_pod(footer_crc32, index_offset);
    crc32_add_pod(footer_crc32, index_size);
    crc32_add_pod(footer_crc32, bloom_offset);
    crc32_add_pod(footer_crc32, bloom_size);
    crc32_add_pod(footer_crc32, meta_offset);
    crc32_add_pod(footer_crc32, meta_size);
    crc32_add_pod(footer_crc32, file_size);
}

SSTable::SSTable(const std::string& path)
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

/// so-called adds"


void SSTableManager::add_to_pool(SSTable& sstable)
{
    this->immutable_pool.emplace_back(sstable);
}

/// block write related

void ensure_fits_in_block(uint64_t& offset, std::size_t size, const uint32_t BLOCK_SIZE, std::ofstream& file)
{
    static uint8_t poison[4096] = { 0xCD };
    if ((offset % BLOCK_SIZE) + size <= BLOCK_SIZE)
        return;
    file.write(reinterpret_cast<const char*>(poison), BLOCK_SIZE - (offset % BLOCK_SIZE));
    offset += BLOCK_SIZE - (offset % BLOCK_SIZE);
}
void write_to_stream(std::ofstream& file, uint64_t& offset, void* ptr, std::size_t size, const uint32_t BLOCK_SIZE)
{
    ensure_fits_in_block(offset, size, BLOCK_SIZE, file);
    file.write(reinterpret_cast<const char*>(ptr), size);
    offset += size;
}
void align_to_block_boundary(uint64_t& offset, const uint32_t BLOCK_SIZE, std::ofstream& file)
{
    ensure_fits_in_block(offset, BLOCK_SIZE, BLOCK_SIZE, file);
}

/// block load related

bool fits_in_block(std::ifstream& file, std::size_t size, const uint32_t BLOCK_SIZE)
{
    uint64_t current = file.tellg();
    if (current / BLOCK_SIZE == (current + size - 1) / BLOCK_SIZE) 
        return true;
    return false;
}
void align_to_block_boundary(std::ifstream& file, const uint32_t BLOCK_SIZE)
{
    uint64_t current = file.tellg();
    uint64_t in_block_offset = current % BLOCK_SIZE;

    if (in_block_offset == 0) return;

    file.seekg(current + (BLOCK_SIZE - in_block_offset), std::ios::beg);
}
void ensure_fits_in_block(std::ifstream& file, std::size_t size, const uint32_t BLOCK_SIZE)
{
    if (fits_in_block(file, size, BLOCK_SIZE)) return;
    align_to_block_boundary(file, BLOCK_SIZE);
}
void move_g_to_next_block(std::ifstream& file, const uint32_t BLOCK_SIZE)
{
    uint64_t current = file.tellg();
    uint64_t in_block_offset = current % BLOCK_SIZE;

    file.seekg(current + (BLOCK_SIZE - in_block_offset), std::ios::beg);
}

/// writes


void SSTable::write()
{
    file_out = std::ofstream(path, std::ios::binary | std::ios::trunc);
    if (!file_out)
        throw std::runtime_error("failed to open SSTable for write");

    uint64_t offset = 0;

    file_header_section.write(file_out, offset);

    // Data write also fills index_block
    uint64_t data_offset = 0;
    data_section.write(file_out, offset, index_section, data_offset);

    // Build bloom/meta after data/index are finalized
    bloom_section.rebuild(data_section);
    meta_section.rebuild(data_section, index_section);

    uint64_t index_offset = 0;
    uint64_t bloom_offset = 0;
    uint64_t meta_offset = 0;

    index_section.write(file_out, offset, index_offset);
    bloom_section.write(file_out, offset, bloom_offset);
    meta_section.write(file_out, offset, meta_offset);

    file_footer_section.data_offset = data_offset;
    file_footer_section.data_block_count = data_section.data_blocks.size();

    file_footer_section.index_offset = index_offset;
    file_footer_section.index_size = static_cast<uint32_t>(index_section.disk_size());

    file_footer_section.bloom_offset = bloom_offset;
    file_footer_section.bloom_size = static_cast<uint32_t>(bloom_section.disk_size());

    file_footer_section.meta_offset = meta_offset;
    file_footer_section.meta_size = static_cast<uint32_t>(meta_section.disk_size());

    file_footer_section.finalize(offset);
    file_footer_section.write(file_out, offset);

    file_out.flush();
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



/// loading



std::optional<SSTable> SSTableLoader::load(const std::string& path, Arena& arena)
{
    SSTable result(path);
    std::ifstream file(path, std::ios::binary | std::ios::beg);
    
    auto file_header_opt = FileHeaderSection::load(file);
    if (!file_header_opt) return std::nullopt;
    auto file_header_section = std::move(*file_header_opt);

    auto file_footer_opt = FileFooterSection::load(file, FileFooterSection::disk_size(), std::ios::end);
    if (!file_footer_opt) return std::nullopt;
    auto file_footer_section = std::move(*file_footer_opt);

    auto data_opt = DataSection::load(file, file_footer_section.data_offset, file_footer_section.data_block_count);
    if (!data_opt) return std::nullopt;
    auto data_section = std::move(*data_opt);

    auto index_opt = IndexSection::load(file, arena, file_footer_section.index_offset);
    if (!index_opt) return std::nullopt;
    auto index_section = std::move(*index_opt);

    auto bloom_opt = BloomSection::load(file, file_footer_section.bloom_offset);
    if (!bloom_opt) return std::nullopt;
    auto bloom_section = std::move(*bloom_opt);

    auto meta_opt = MetaSection::load(file, data_section, index_section, file_footer_section.meta_offset);
    if (!meta_opt) return std::nullopt;
    auto meta_section = std::move(*meta_opt);

    result.file_header_section = std::move(file_header_section);
    result.data_section = std::move(data_section);
    result.index_section = std::move(index_section);
    result.bloom_section = std::move(bloom_section);
    result.meta_section = std::move(meta_section);
    result.file_footer_section = std::move(file_footer_section);

    return result;
}
void SSTableManager::load(Arena& arena)
{
    while (true)
    {
        std::string next_name = std::move(get_next_name());
        if (next_name.empty()) break;

        auto sstable = this->sstable_loader.load(next_name, arena);
        if (!sstable) continue;

        this->immutable_pool.emplace_back(std::move(*sstable));
    }
}



/// DataSection 


void DataSection::DataBlock::add_payload(Payload& payload)
{
    header.payload_disk_size += payload.disk_size();

    // CRC of logical serialized content
    crc32_add_pod(header.crc32, payload.key_size);
    crc32_add_pod(header.crc32, payload.value_size);
    crc32_add_pod(header.crc32, payload.type);
    crc32_add_pod(header.crc32, payload.flags);
    crc32_add_pod(header.crc32, payload.reserved);
    crc32_add_pod(header.crc32, payload.seq_num);

    if (payload.key_ptr && payload.key_size > 0)
        compute_crc32(header.crc32, payload.key_ptr, payload.key_size);

    if (payload.value_ptr && payload.value_size > 0)
        compute_crc32(header.crc32, payload.value_ptr, payload.value_size);

    payloads.emplace_back(payload);
}
void DataSection::add_payload(const InternalRecord& record)
{
    Payload payload{};
    payload.key_size = static_cast<uint32_t>(record.key_entry.size);
    payload.value_size = static_cast<uint32_t>(record.value_entry.size);
    payload.type = record.type;
    payload.flags = 0;
    payload.reserved = 0;
    payload.seq_num = record.seq_num;
    payload.key_ptr = record.key_entry.data;
    payload.value_ptr = record.value_entry.data;

    if (!data_blocks.back().can_payload_fit(payload))
        init_new_block();

    data_blocks.back().add_payload(payload);
}

void DataSection::DataBlock::write(std::ofstream& file, uint64_t& offset, IndexSection& index_section) {
    align_to_block_boundary(offset, BLOCK_SIZE, file); // each datablock less or equal to the size of physical block size. it was adjusted during .add;
    
    uint64_t data_block_offset = offset;

    write_to_stream(file, offset, &this->header.type, sizeof(this->header.type), BLOCK_SIZE);
    write_to_stream(file, offset, &this->header.payload_disk_size, sizeof(this->header.payload_disk_size), BLOCK_SIZE);
    write_to_stream(file, offset, &this->header.crc32, sizeof(this->header.crc32), BLOCK_SIZE);

    bool first_key_set = false;
    uint32_t first_key_size = 0, last_key_size = 0;
    void* first_key_ptr = nullptr;
    void* last_key_ptr = nullptr;

    for (auto& payload : this->payloads)
    {
        if (payload.disk_size() > BLOCK_SIZE - Header::disk_size())
            throw std::runtime_error("DataSection payload too large for a block");

        write_to_stream(file, offset, &payload.key_size, sizeof(payload.key_size), BLOCK_SIZE);
        write_to_stream(file, offset, &payload.value_size, sizeof(payload.value_size), BLOCK_SIZE);
        write_to_stream(file, offset, &payload.type, sizeof(payload.type), BLOCK_SIZE);
        write_to_stream(file, offset, &payload.flags, sizeof(payload.flags), BLOCK_SIZE);
        write_to_stream(file, offset, &payload.reserved, sizeof(payload.reserved), BLOCK_SIZE);
        write_to_stream(file, offset, &payload.seq_num, sizeof(payload.seq_num), BLOCK_SIZE);

        uint64_t key_offset = offset;

        if (!first_key_set)
        {
            first_key_ptr = payload.key_ptr;
            first_key_size = payload.key_size;
            first_key_set = true;
        }

        last_key_ptr = payload.key_ptr;
        last_key_size = payload.key_size;

        write_to_stream(file, offset, payload.key_ptr, payload.key_size, BLOCK_SIZE);
        write_to_stream(file, offset, payload.value_ptr, payload.value_size, BLOCK_SIZE);
    }

    if(!payloads.empty())
        index_section.add_index(data_block_offset, first_key_size, last_key_size, first_key_ptr, last_key_ptr);
}
void DataSection::write(std::ofstream& file, uint64_t& offset, IndexSection& index_section, uint64_t& data_offset)
{
    align_to_block_boundary(offset, BLOCK_SIZE, file);
    data_offset = offset;
    for (auto& data_block : this->data_blocks)
    {
        data_block.write(file, offset, index_section);
    }
}

std::optional<DataSection> DataSection::load(std::ifstream& file, uint64_t first_data_block_offset, uint32_t data_block_count)
{
    file.seekg(first_data_block_offset, std::ios::beg);
    if (file.eof()) return std::nullopt;

    DataSection result;
    while (data_block_count--)
    {
        auto data_block = DataSection::DataBlock::load(file);
        if (!data_block) return std::nullopt;
        result.data_blocks.emplace_back(std::move(*data_block));
    }

    return result;
}
std::optional<DataSection::DataBlock> DataSection::DataBlock::load(std::ifstream& file)
{
    DataSection::DataBlock result{};
    align_to_block_boundary(file, BLOCK_SIZE);
    if (file.eof()) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.header.type), sizeof(result.header.type));
    if (!file) return std::nullopt;
    if (result.header.type != BlockType::Data) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.header.payload_disk_size), sizeof(result.header.payload_disk_size));
    if (!file) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.header.crc32), sizeof(result.header.crc32));
    if (!file) return std::nullopt;

    if (result.header.payload_disk_size > BLOCK_SIZE - Header::disk_size())
        return std::nullopt;

    // payload

    uint32_t must_be_crc32 = ::crc32(0L, Z_NULL, 0);
    uint64_t payload_bytes_read = 0;

    while (payload_bytes_read < result.header.payload_disk_size)
    {
        DataSection::Payload payload{};

        file.read(reinterpret_cast<char*>(&payload.key_size), sizeof(payload.key_size));
        if (!file) return std::nullopt;

        file.read(reinterpret_cast<char*>(&payload.value_size), sizeof(payload.value_size));
        if (!file) return std::nullopt;

        file.read(reinterpret_cast<char*>(&payload.type), sizeof(payload.type));
        if (!file) return std::nullopt;

        file.read(reinterpret_cast<char*>(&payload.flags), sizeof(payload.flags));
        if (!file) return std::nullopt;

        file.read(reinterpret_cast<char*>(&payload.reserved), sizeof(payload.reserved));
        if (!file) return std::nullopt;

        file.read(reinterpret_cast<char*>(&payload.seq_num), sizeof(payload.seq_num));
        if (!file) return std::nullopt;

        const uint64_t record_size =
            sizeof(payload.key_size) +
            sizeof(payload.value_size) +
            sizeof(payload.type) +
            sizeof(payload.flags) +
            sizeof(payload.reserved) +
            sizeof(payload.seq_num) +
            payload.key_size +
            payload.value_size;

        if (record_size > result.header.payload_disk_size - payload_bytes_read)
            return std::nullopt;
        if (payload.key_size > BLOCK_SIZE)
            return std::nullopt;
        if (payload.value_size > BLOCK_SIZE)
            return std::nullopt;

        std::vector<std::byte> key_buf(payload.key_size);
        std::vector<std::byte> value_buf(payload.value_size);

        file.read(reinterpret_cast<char*>(key_buf.data()), payload.key_size);
        if (!file) return std::nullopt;

        file.read(reinterpret_cast<char*>(value_buf.data()), payload.value_size);
        if (!file) return std::nullopt;

        payload.key_ptr = nullptr;
        payload.value_ptr = nullptr; // the acutal data wouldn't be loaded (since it takes too much ram)

        crc32_add_pod(must_be_crc32, payload.key_size);
        crc32_add_pod(must_be_crc32, payload.value_size);
        crc32_add_pod(must_be_crc32, payload.type);
        crc32_add_pod(must_be_crc32, payload.flags);
        crc32_add_pod(must_be_crc32, payload.reserved);
        crc32_add_pod(must_be_crc32, payload.seq_num);

        compute_crc32(must_be_crc32, key_buf.data(), payload.key_size);
        compute_crc32(must_be_crc32, value_buf.data(), payload.value_size);

        result.payloads.emplace_back(std::move(payload));
        payload_bytes_read += record_size;
    }

    if (payload_bytes_read != result.header.payload_disk_size)
        return std::nullopt;

    if (must_be_crc32 != result.header.crc32) return std::nullopt;

    return result;
}



// FileHeaderSection



void FileHeaderSection::write(std::ofstream& file, uint64_t& offset)
{
    ensure_fits_in_block(offset, FileHeaderSection::disk_size(), BLOCK_SIZE, file);
    write_to_stream(file, offset, &this->magic, sizeof(this->magic), BLOCK_SIZE);
    write_to_stream(file, offset, &this->version, sizeof(this->version), BLOCK_SIZE);
    write_to_stream(file, offset, &this->flags, sizeof(this->flags), BLOCK_SIZE);
    write_to_stream(file, offset, &this->block_size, sizeof(this->block_size), BLOCK_SIZE);
    write_to_stream(file, offset, &this->table_id, sizeof(this->table_id), BLOCK_SIZE);
    write_to_stream(file, offset, &this->crc32, sizeof(this->crc32), BLOCK_SIZE);
}

std::optional<FileHeaderSection> FileHeaderSection::load(std::ifstream& file)
{
    FileHeaderSection result{};
    ensure_fits_in_block(file, FileHeaderSection::disk_size(), BLOCK_SIZE);
    uint64_t file_header_offset = file.tellg();

    file.read(reinterpret_cast<char*>(&result.magic), sizeof(result.magic));
    if (!file) return std::nullopt;
    if (result.magic != FILE_HEADER_MAGIC) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.version), sizeof(result.version));
    if (!file) return std::nullopt;
    if (result.version != SSTABLE_VERSION) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.flags), sizeof(result.flags));
    if (!file) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.block_size), sizeof(result.block_size));
    if (!file) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.table_id), sizeof(result.table_id));
    if (!file) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.crc32), sizeof(result.crc32));
    if (!file) return std::nullopt;

    uint32_t must_be_crc32 = ::crc32(0L, Z_NULL, 0);
    crc32_add_pod(must_be_crc32, result.magic);
    crc32_add_pod(must_be_crc32, result.version);
    crc32_add_pod(must_be_crc32, result.flags);
    crc32_add_pod(must_be_crc32, result.block_size);
    crc32_add_pod(must_be_crc32, result.table_id);

    if (must_be_crc32 != result.crc32) return std::nullopt;
    if (result.block_size != BLOCK_SIZE) return std::nullopt;
    return result;
}



// IndexSection



std::optional<IndexSection> IndexSection::load(std::ifstream& file, Arena& arena, uint64_t index_offset)
{
    if (index_offset)
    {
        file.seekg(index_offset, std::ios::beg);
        if (!file) return std::nullopt;
    }

    IndexSection result{};
    uint64_t index_block_offset = file.tellg();
    
    file.read(reinterpret_cast<char*>(&result.header.type), sizeof(result.header.type));
    if (!file) return std::nullopt;
    if (result.header.type != BlockType::Index) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.header.payload_size), sizeof(result.header.payload_size));
    if (!file) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.header.crc32), sizeof(result.header.crc32));
    if (!file) return std::nullopt;

    // payload

    uint32_t must_be_crc32 = ::crc32(0L, Z_NULL, 0);
    uint64_t payload_bytes_read = 0;
    while (payload_bytes_read < result.header.payload_size)
    {
        IndexSection::Payload payload{};

        file.read(reinterpret_cast<char*>(&payload.data_block_offset), sizeof(payload.data_block_offset));
        if (!file) return std::nullopt;

        file.read(reinterpret_cast<char*>(&payload.first_key_size), sizeof(payload.first_key_size));
        if (!file) return std::nullopt;

        file.read(reinterpret_cast<char*>(&payload.last_key_size), sizeof(payload.last_key_size));
        if (!file) return std::nullopt;


        const uint64_t record_size =
            sizeof(payload.first_key_size) +
            sizeof(payload.last_key_size) +
            sizeof(payload.data_block_offset) +
            payload.first_key_size +
            payload.last_key_size;

        if (record_size > result.header.payload_size - payload_bytes_read)
            return std::nullopt;
        if (payload.first_key_size > BLOCK_SIZE)
            return std::nullopt;
        if (payload.last_key_size > BLOCK_SIZE)
            return std::nullopt;

        void* first_key_ptr = arena.alloc(payload.first_key_size, alignof(std::byte)); // Arena arena will be handled and provided in some other place
        void* last_key_ptr = arena.alloc(payload.last_key_size, alignof(std::byte));

        if (payload.first_key_size > 0 && first_key_ptr == nullptr)
            return std::nullopt;
        if (payload.last_key_size > 0 && last_key_ptr == nullptr)
            return std::nullopt;

        file.read(reinterpret_cast<char*>(first_key_ptr), payload.first_key_size);
        if (!file) return std::nullopt;

        file.read(reinterpret_cast<char*>(last_key_ptr), payload.last_key_size);
        if (!file) return std::nullopt;

        payload.first_key_ptr = first_key_ptr;
        payload.last_key_ptr = last_key_ptr;

        crc32_add_pod(must_be_crc32, payload.data_block_offset);
        crc32_add_pod(must_be_crc32, payload.first_key_size);
        crc32_add_pod(must_be_crc32, payload.last_key_size);

        compute_crc32(must_be_crc32, payload.first_key_ptr, payload.first_key_size);
        compute_crc32(must_be_crc32, payload.last_key_ptr, payload.last_key_size);

        result.payloads.emplace_back(std::move(payload));
        payload_bytes_read += record_size;
    }

    if (payload_bytes_read != result.header.payload_size)
        return std::nullopt;

    if (must_be_crc32 != result.header.crc32) return std::nullopt;

    return result;
}

void IndexSection::write(std::ofstream& file, uint64_t& offset, uint64_t& index_offset)
{
    align_to_block_boundary(offset, BLOCK_SIZE, file);
    index_offset = offset;
    write_to_stream(file, offset, &this->header.type, sizeof(this->header.type), BLOCK_SIZE);
    write_to_stream(file, offset, &this->header.payload_size, sizeof(this->header.payload_size), BLOCK_SIZE);
    write_to_stream(file, offset, &this->header.crc32, sizeof(this->header.crc32), BLOCK_SIZE);
    for (auto& payload : this->payloads)
    {
        ensure_fits_in_block(offset, payload.disk_size(), BLOCK_SIZE, file);
        write_to_stream(file, offset, &payload.data_block_offset, sizeof(payload.data_block_offset), BLOCK_SIZE);
        write_to_stream(file, offset, &payload.first_key_size, sizeof(payload.first_key_size), BLOCK_SIZE);
        write_to_stream(file, offset, &payload.last_key_size, sizeof(payload.last_key_size), BLOCK_SIZE);
        write_to_stream(file, offset, payload.first_key_ptr, payload.first_key_size, BLOCK_SIZE);
        write_to_stream(file, offset, payload.last_key_ptr, payload.last_key_size, BLOCK_SIZE);
    }
}

void IndexSection::add_index(
    uint64_t data_block_offset,
    uint32_t first_key_size,
    uint32_t last_key_size,
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

    crc32_add_pod(this->header.crc32, payload.data_block_offset);
    crc32_add_pod(this->header.crc32, payload.first_key_size);
    crc32_add_pod(this->header.crc32, payload.last_key_size);

    if (payload.first_key_ptr && payload.first_key_size > 0)
        compute_crc32(this->header.crc32, payload.first_key_ptr, payload.first_key_size);

    if (payload.last_key_ptr && payload.last_key_size > 0)
        compute_crc32(this->header.crc32, payload.last_key_ptr, payload.last_key_size);
}



// BloomSection


std::optional<BloomSection> BloomSection::load(std::ifstream& file, uint64_t bloom_offset)
{
    if (bloom_offset)
    {
        file.seekg(bloom_offset, std::ios::beg);
        if (!file) return std::nullopt;
    } 

    BloomSection result{};
    uint64_t bloom_block_offset = file.tellg();

    file.read(reinterpret_cast<char*>(&result.header.type), sizeof(result.header.type));
    if (!file) return std::nullopt;
    if (result.header.type != BlockType::Bloom) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.header.payload_size), sizeof(result.header.payload_size));
    if (!file) return std::nullopt;
    if (result.header.payload_size != BloomSection::Payload::disk_size()) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.header.crc32), sizeof(result.header.crc32));
    if (!file) return std::nullopt;

    uint64_t payload_size = 0;

    file.read(reinterpret_cast<char*>(&result.payload.bloom_bits), sizeof(result.payload.bloom_bits));
    if (!file) return std::nullopt;
    payload_size += sizeof(result.payload.bloom_bits);

    file.read(reinterpret_cast<char*>(&result.payload.hash_count), sizeof(result.payload.hash_count));
    if (!file) return std::nullopt;
    payload_size += sizeof(result.payload.hash_count);

    file.read(reinterpret_cast<char*>(&result.payload.key_count), sizeof(result.payload.key_count));
    if (!file) return std::nullopt;
    payload_size += sizeof(result.payload.key_count);

    file.read(reinterpret_cast<char*>(&result.payload.mask), sizeof(result.payload.mask));
    if (!file) return std::nullopt;
    payload_size += sizeof(result.payload.mask);

    uint32_t must_be_crc32 = ::crc32(0L, Z_NULL, 0);
    crc32_add_pod(must_be_crc32, result.payload.bloom_bits);
    crc32_add_pod(must_be_crc32, result.payload.hash_count);
    crc32_add_pod(must_be_crc32, result.payload.key_count);

    compute_crc32(must_be_crc32, &result.payload.mask, sizeof(result.payload.mask));

    if (must_be_crc32 != result.header.crc32) return std::nullopt;
    if (result.payload.hash_count != BLOOM_HASH_COUNT) return std::nullopt;
    if (payload_size != result.header.payload_size) return std::nullopt;
    if (result.payload.bloom_bits != result.payload.mask.size()) return std::nullopt;

    return result;
}

void BloomSection::write(std::ofstream& file, uint64_t& offset, uint64_t& bloom_offset)
{
    align_to_block_boundary(offset, BLOCK_SIZE, file);
    bloom_offset = offset;
    write_to_stream(file, offset, &this->header.type, sizeof(this->header.type), BLOCK_SIZE);
    write_to_stream(file, offset, &this->header.payload_size, sizeof(this->header.payload_size), BLOCK_SIZE);
    write_to_stream(file, offset, &this->header.crc32, sizeof(this->header.crc32), BLOCK_SIZE);

    write_to_stream(file, offset, &this->payload.bloom_bits, sizeof(this->payload.bloom_bits), BLOCK_SIZE);
    write_to_stream(file, offset, &this->payload.hash_count, sizeof(this->payload.hash_count), BLOCK_SIZE);
    write_to_stream(file, offset, &this->payload.key_count, sizeof(this->payload.key_count), BLOCK_SIZE);

    write_to_stream(file, offset, &this->payload.mask, sizeof(this->payload.mask), BLOCK_SIZE);
}

void BloomSection::add_key(const void* key_ptr, uint32_t key_size)
{
    if (!key_ptr || key_size == 0)
        return;

    for (uint32_t i = 0; i < payload.hash_count; ++i) {
        uint64_t h = bloom_hash_i(key_ptr, key_size, i);
        uint64_t bit = h % payload.bloom_bits;
        payload.mask.set(bit);
    }

    ++payload.key_count;
}

bool BloomSection::may_contain(const void* key_ptr, uint32_t key_size) const
{
    if (!key_ptr || !key_size) return false;

    for (std::size_t i = 0; i < payload.hash_count; i++)
    {
        uint64_t h = bloom_hash_i(key_ptr, key_size, i);
        uint64_t bit = h % payload.bloom_bits;

        if (!payload.mask.test(bit))
            return false;
    }

    return true;
}

void BloomSection::recompute_crc32()
{
    header.type = BlockType::Bloom;
    header.payload_size = Payload::disk_size();
    header.crc32 = ::crc32(0L, Z_NULL, 0);

    crc32_add_pod(header.crc32, payload.bloom_bits);
    crc32_add_pod(header.crc32, payload.hash_count);
    crc32_add_pod(header.crc32, payload.key_count);
    compute_crc32(header.crc32, &payload.mask, sizeof(payload.mask));
}

void BloomSection::rebuild(const DataSection& data_section)
{
    payload.bloom_bits = static_cast<uint32_t>(payload.mask.size());   
    payload.hash_count = BLOOM_HASH_COUNT;
    payload.key_count = 0;
    payload.mask.reset();

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


void MetaSection::write(std::ofstream& file, uint64_t& offset, uint64_t& meta_offset)
{
    align_to_block_boundary(offset, BLOCK_SIZE, file);
    meta_offset = offset;

    write_to_stream(file, offset, &this->header.type, sizeof(this->header.type), BLOCK_SIZE);
    write_to_stream(file, offset, &this->header.payload_size, sizeof(this->header.payload_size), BLOCK_SIZE);
    write_to_stream(file, offset, &this->header.crc32, sizeof(this->header.crc32), BLOCK_SIZE);

    write_to_stream(file, offset, &this->payload.record_count, sizeof(this->payload.record_count), BLOCK_SIZE);
    write_to_stream(file, offset, &this->payload.tombstone_count, sizeof(this->payload.tombstone_count), BLOCK_SIZE);
    write_to_stream(file, offset, &this->payload.min_seq_num, sizeof(this->payload.min_seq_num), BLOCK_SIZE);
    write_to_stream(file, offset, &this->payload.max_seq_num, sizeof(this->payload.max_seq_num), BLOCK_SIZE);
    write_to_stream(file, offset, &this->payload.min_key_size, sizeof(this->payload.min_key_size), BLOCK_SIZE);
    write_to_stream(file, offset, &this->payload.max_key_size, sizeof(this->payload.max_key_size), BLOCK_SIZE);
    write_to_stream(file, offset, &this->payload.data_block_count, sizeof(this->payload.data_block_count), BLOCK_SIZE);
    write_to_stream(file, offset, &this->payload.data_bytes, sizeof(this->payload.data_bytes), BLOCK_SIZE);
}

std::optional<MetaSection> MetaSection::load(
    std::ifstream& file,
    DataSection& data_section,
    IndexSection& index_section,
    uint64_t meta_offset
) {
    if (meta_offset)
    {
        file.seekg(meta_offset, std::ios::beg);
        if (!file) return std::nullopt;
    }

    MetaSection result{};
    uint64_t meta_block_offset = file.tellg();
    
    file.read(reinterpret_cast<char*>(&result.header.type), sizeof(result.header.type));
    if (!file) return std::nullopt;
    if (result.header.type != BlockType::Meta) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.header.payload_size), sizeof(result.header.payload_size));
    if (!file) return std::nullopt;
    if (result.header.payload_size != MetaSection::Payload::disk_size()) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.header.crc32), sizeof(result.header.crc32));
    if (!file) return std::nullopt;

    uint64_t payload_size = 0;

    file.read(reinterpret_cast<char*>(&result.payload.record_count), sizeof(result.payload.record_count));
    if (!file) return std::nullopt;
    payload_size += sizeof(result.payload.record_count);

    file.read(reinterpret_cast<char*>(&result.payload.tombstone_count), sizeof(result.payload.tombstone_count));
    if (!file) return std::nullopt;
    payload_size += sizeof(result.payload.tombstone_count);

    file.read(reinterpret_cast<char*>(&result.payload.min_seq_num), sizeof(result.payload.min_seq_num));
    if (!file) return std::nullopt;
    payload_size += sizeof(result.payload.min_seq_num);

    file.read(reinterpret_cast<char*>(&result.payload.max_seq_num), sizeof(result.payload.max_seq_num));
    if (!file) return std::nullopt;
    payload_size += sizeof(result.payload.max_seq_num);

    file.read(reinterpret_cast<char*>(&result.payload.min_key_size), sizeof(result.payload.min_key_size));
    if (!file) return std::nullopt;
    payload_size += sizeof(result.payload.min_key_size);

    file.read(reinterpret_cast<char*>(&result.payload.max_key_size), sizeof(result.payload.max_key_size));
    if (!file) return std::nullopt;
    payload_size += sizeof(result.payload.max_key_size);

    file.read(reinterpret_cast<char*>(&result.payload.data_block_count), sizeof(result.payload.data_block_count));
    if (!file) return std::nullopt;
    if (result.payload.data_block_count != index_section.payloads.size()) return std::nullopt;
    payload_size += sizeof(result.payload.data_block_count);

    file.read(reinterpret_cast<char*>(&result.payload.data_bytes), sizeof(result.payload.data_bytes));
    if (!file) return std::nullopt;
    payload_size += sizeof(result.payload.data_bytes);

    uint32_t must_be_crc32 = ::crc32(0L, Z_NULL, 0);
    crc32_add_pod(must_be_crc32, result.payload.record_count);
    crc32_add_pod(must_be_crc32, result.payload.tombstone_count);
    crc32_add_pod(must_be_crc32, result.payload.min_seq_num);
    crc32_add_pod(must_be_crc32, result.payload.max_seq_num);
    crc32_add_pod(must_be_crc32, result.payload.min_key_size);
    crc32_add_pod(must_be_crc32, result.payload.max_key_size);
    crc32_add_pod(must_be_crc32, result.payload.data_block_count);
    crc32_add_pod(must_be_crc32, result.payload.data_bytes);

    if (must_be_crc32 != result.header.crc32) return std::nullopt;

    uint32_t must_be_tombstone_count = 0;
    uint32_t must_be_min_key_size = std::numeric_limits<uint32_t>::max(), must_be_max_key_size = std::numeric_limits<uint32_t>::min();
    uint64_t must_be_min_seq_num = std::numeric_limits<uint64_t>::max(), must_be_max_seq_num = std::numeric_limits<uint64_t>::min();
    uint64_t must_be_data_bytes = 0;

    for (const auto& block : data_section.data_blocks)
    {
        
        for (const auto& payload : block.payloads)
        {
            must_be_tombstone_count += (payload.type == Type::Tombstone);
            must_be_min_key_size = std::min(must_be_min_key_size, payload.key_size);
            must_be_max_key_size = std::max(must_be_max_key_size, payload.key_size);
            must_be_min_seq_num = std::min(must_be_min_seq_num, payload.seq_num);
            must_be_max_seq_num = std::max(must_be_max_seq_num, payload.seq_num);
            must_be_data_bytes += payload.disk_size();
        }
    }

    if (data_section.data_blocks.empty())
    {
        must_be_min_key_size = 0;
        must_be_max_key_size = 0;
        must_be_min_seq_num = 0;
        must_be_max_seq_num = 0;
    }

    if (result.payload.tombstone_count != must_be_tombstone_count) return std::nullopt;
    if (result.payload.min_key_size != must_be_min_key_size) return std::nullopt;
    if (result.payload.max_key_size != must_be_max_key_size) return std::nullopt;
    if (result.payload.min_seq_num != must_be_min_seq_num) return std::nullopt;
    if (result.payload.max_seq_num != must_be_max_seq_num) return std::nullopt;
    if (result.payload.data_bytes != must_be_data_bytes) return std::nullopt;
    if (result.header.payload_size != payload_size) return std::nullopt;

    return result;
}

void MetaSection::rebuild(DataSection& data_section, IndexSection& index_section)
{
    this->payload.record_count = 0;
    this->payload.tombstone_count = 0;
    this->payload.min_seq_num = std::numeric_limits<uint64_t>::max();
    this->payload.max_seq_num = 0;
    this->payload.min_key_size = std::numeric_limits<uint32_t>::max();
    this->payload.max_key_size = 0;
    this->payload.data_block_count = 0;
    this->payload.data_bytes = 0;

    this->payload.data_block_count = data_section.data_blocks.size();

    for (const auto& block : data_section.data_blocks)
    {
        this->payload.data_bytes += block.disk_size();
        for (const auto& payload : block.payloads)
        {
            ++this->payload.record_count;
            this->payload.tombstone_count += (payload.type == Type::Tombstone);
            this->payload.min_key_size = std::min(this->payload.min_key_size, payload.key_size);
            this->payload.max_key_size = std::max(this->payload.max_key_size, payload.key_size);
            this->payload.min_seq_num = std::min(this->payload.min_seq_num, payload.seq_num);
            this->payload.max_seq_num = std::max(this->payload.max_seq_num, payload.seq_num);
        }
    }

    if (data_section.data_blocks.empty())
    {
        this->payload.min_seq_num = 0;
        this->payload.max_seq_num = 0;
        this->payload.min_key_size = 0;
        this->payload.max_key_size = 0;
    }

    this->header.payload_size = Payload::disk_size();
    this->header.crc32 = ::crc32(0L, Z_NULL, 0);

    crc32_add_pod(this->header.crc32, this->payload.record_count);
    crc32_add_pod(this->header.crc32, this->payload.tombstone_count);
    crc32_add_pod(this->header.crc32, this->payload.min_seq_num);
    crc32_add_pod(this->header.crc32, this->payload.max_seq_num);
    crc32_add_pod(this->header.crc32, this->payload.min_key_size);
    crc32_add_pod(this->header.crc32, this->payload.max_key_size);
    crc32_add_pod(this->header.crc32, this->payload.data_block_count);
    crc32_add_pod(this->header.crc32, this->payload.data_bytes);
}



// FileFooterSection


void FileFooterSection::write(std::ofstream& file, uint64_t& offset)
{
    ensure_fits_in_block(offset, FileFooterSection::disk_size(), BLOCK_SIZE, file);
    write_to_stream(file, offset, &this->magic, sizeof(this->magic), BLOCK_SIZE);
    write_to_stream(file, offset, &this->version, sizeof(this->version), BLOCK_SIZE);
    write_to_stream(file, offset, &this->reserved, sizeof(this->reserved), BLOCK_SIZE);

    write_to_stream(file, offset, &this->data_offset, sizeof(this->data_offset), BLOCK_SIZE);
    write_to_stream(file, offset, &this->data_block_count, sizeof(this->data_block_count), BLOCK_SIZE);

    write_to_stream(file, offset, &this->index_offset, sizeof(this->index_offset), BLOCK_SIZE);
    write_to_stream(file, offset, &this->index_size, sizeof(this->index_size), BLOCK_SIZE);

    write_to_stream(file, offset, &this->bloom_offset, sizeof(this->bloom_offset), BLOCK_SIZE);
    write_to_stream(file, offset, &this->bloom_size, sizeof(this->bloom_size), BLOCK_SIZE);

    write_to_stream(file, offset, &this->meta_offset, sizeof(this->meta_offset), BLOCK_SIZE);
    write_to_stream(file, offset, &this->meta_size, sizeof(this->meta_size), BLOCK_SIZE);

    write_to_stream(file, offset, &this->file_size, sizeof(this->file_size), BLOCK_SIZE);
    write_to_stream(file, offset, &this->footer_crc32, sizeof(this->footer_crc32), BLOCK_SIZE);
}

void FileFooterSection::finalize(uint64_t current_offset)
{
    this->file_size = current_offset + FileFooterSection::disk_size();

    this->footer_crc32 = ::crc32(0L, Z_NULL, 0);
    crc32_add_pod(this->footer_crc32, this->magic);
    crc32_add_pod(this->footer_crc32, this->version);
    crc32_add_pod(this->footer_crc32, this->reserved);
    crc32_add_pod(this->footer_crc32, this->data_offset);
    crc32_add_pod(this->footer_crc32, this->data_block_count);
    crc32_add_pod(this->footer_crc32, this->index_offset);
    crc32_add_pod(this->footer_crc32, this->index_size);
    crc32_add_pod(this->footer_crc32, this->bloom_offset);
    crc32_add_pod(this->footer_crc32, this->bloom_size);
    crc32_add_pod(this->footer_crc32, this->meta_offset);
    crc32_add_pod(this->footer_crc32, this->meta_size);
    crc32_add_pod(this->footer_crc32, this->file_size);
}

std::optional<FileFooterSection> FileFooterSection::load(
    std::ifstream& file,
    uint64_t file_footer_offset,
    auto dir
) {
    if (file_footer_offset)
    {
        if(dir == std::ios::beg)
            file.seekg(file_footer_offset, dir);
        else
            file.seekg(-static_cast<std::streamoff>(FileFooterSection::disk_size()), std::ios::end);
        if (!file) return std::nullopt;
    }

    FileFooterSection result{};

    file.read(reinterpret_cast<char*>(&result.magic), sizeof(result.magic));
    if (!file) return std::nullopt;
    if (result.magic != FILE_FOOTER_MAGIC) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.version), sizeof(result.version));
    if (!file) return std::nullopt;
    if (result.version != SSTABLE_VERSION) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.reserved), sizeof(result.reserved));
    if (!file) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.data_offset), sizeof(result.data_offset));
    if (!file) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.data_block_count), sizeof(result.data_block_count));
    if (!file) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.index_offset), sizeof(result.index_offset));
    if (!file) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.index_size), sizeof(result.index_size));
    if (!file) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.bloom_offset), sizeof(result.bloom_offset));
    if (!file) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.bloom_size), sizeof(result.bloom_size));
    if (!file) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.meta_offset), sizeof(result.meta_offset));
    if (!file) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.meta_size), sizeof(result.meta_size));
    if (!file) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.file_size), sizeof(result.file_size));
    if (!file) return std::nullopt;

    auto cur = file.tellg();
    file.seekg(0, std::ios::end);
    auto actual_file_size = file.tellg();
    file.seekg(cur);
    if (result.file_size != static_cast<uint64_t>(actual_file_size)) return std::nullopt;

    file.read(reinterpret_cast<char*>(&result.footer_crc32), sizeof(result.footer_crc32));
    if (!file) return std::nullopt;

    uint32_t must_be_footer_crc32 = ::crc32(0L, Z_NULL, 0);
    crc32_add_pod(must_be_footer_crc32, result.magic);
    crc32_add_pod(must_be_footer_crc32, result.version);
    crc32_add_pod(must_be_footer_crc32, result.reserved);
    crc32_add_pod(must_be_footer_crc32, result.data_offset);
    crc32_add_pod(must_be_footer_crc32, result.data_block_count);
    crc32_add_pod(must_be_footer_crc32, result.index_offset);
    crc32_add_pod(must_be_footer_crc32, result.index_size);
    crc32_add_pod(must_be_footer_crc32, result.bloom_offset);
    crc32_add_pod(must_be_footer_crc32, result.bloom_size);
    crc32_add_pod(must_be_footer_crc32, result.meta_offset);
    crc32_add_pod(must_be_footer_crc32, result.meta_size);
    crc32_add_pod(must_be_footer_crc32, result.file_size);

    if (must_be_footer_crc32 != result.footer_crc32) return std::nullopt;

    return result;
}

void FileFooterSection::rebuild(IndexSection& index_block, uint64_t index_offset)
{
    this->index_offset = index_offset;
    this->index_size = static_cast<uint32_t>(index_block.disk_size());
}
void FileFooterSection::rebuild(BloomSection& bloom_block, uint64_t bloom_offset)
{
    this->bloom_offset = bloom_offset;
    this->bloom_size = static_cast<uint32_t>(BloomSection::disk_size());
}
void FileFooterSection::rebuild(MetaSection& meta_block, uint64_t meta_offset)
{
    this->meta_offset = meta_offset;
    this->meta_size = static_cast<uint32_t>(MetaSection::disk_size());
}


///*

//SSTable::SSTable(const MemTable& mem_table, uint32_t table_id)
//    : file_header(table_id),
//    data_block(),
//    index_block(),
//    bloom_block(),
//    meta_block(),
//    file_footer()
//{
//    std::vector<InternalRecord> records = mem_table.get_oldest_table();
//
//    if (records.empty())
//        return;
//
//    meta_block.payload.record_count = records.size();
//    meta_block.payload.min_seq_num = std::numeric_limits<uint64_t>::max();
//    meta_block.payload.max_seq_num = 0;
//    meta_block.payload.min_key_size = std::numeric_limits<uint32_t>::max();
//    meta_block.payload.max_key_size = 0;
//    meta_block.payload.data_block_count = 1; // for now: single logical data block
//    meta_block.payload.data_bytes = 0;
//
//    for (const auto& record : records)
//    {
//        data_block.add_data(record);
//
//        const uint32_t key_size = static_cast<uint32_t>(record.key_entry.size);
//        const uint32_t value_size = static_cast<uint32_t>(record.value_entry.size);
//
//        if (record.type == ::Type::Tombstone)
//            ++meta_block.payload.tombstone_count;
//
//        meta_block.payload.min_seq_num = std::min(meta_block.payload.min_seq_num, record.seq_num);
//        meta_block.payload.max_seq_num = std::max(meta_block.payload.max_seq_num, record.seq_num);
//        meta_block.payload.min_key_size = std::min(meta_block.payload.min_key_size, key_size);
//        meta_block.payload.max_key_size = std::max(meta_block.payload.max_key_size, key_size);
//
//        meta_block.payload.data_bytes +=
//            sizeof(uint32_t) + // key_size
//            sizeof(uint32_t) + // value_size
//            sizeof(::Type) +
//            sizeof(uint32_t) + // flags
//            sizeof(uint32_t) + // reserved
//            sizeof(uint64_t) + // seq_num
//            key_size +
//            value_size;
//
//        // Later you should add bloom filter updates here
//        // bloom_block.add_key(...)
//    }
//
//    // Build one simple index entry covering the whole data block for now
//    {
//        IndexSection::Payload idx{};
//        idx.first_key_size = static_cast<uint32_t>(records.front().key_entry.size);
//        idx.last_key_size = static_cast<uint32_t>(records.back().key_entry.size);
//        idx.first_key_offset = 0; // for now, whole block starts at offset 0
//        idx.first_key_ptr = records.front().key_entry.data;
//        idx.last_key_ptr = records.back().key_entry.data;
//
//        index_block.payloads.emplace_back(idx);
//
//        index_block.header.payload_size +=
//            sizeof(idx.first_key_size) +
//            sizeof(idx.last_key_size) +
//            sizeof(idx.first_key_offset) +
//            idx.first_key_size +
//            idx.last_key_size;
//
//        crc32_add_pod(index_block.header.crc32, idx.first_key_size);
//        crc32_add_pod(index_block.header.crc32, idx.last_key_size);
//        crc32_add_pod(index_block.header.crc32, idx.first_key_offset);
//
//        if (idx.first_key_ptr && idx.first_key_size > 0)
//            compute_crc32(index_block.header.crc32, idx.first_key_ptr, idx.first_key_size);
//
//        if (idx.last_key_ptr && idx.last_key_size > 0)
//            compute_crc32(index_block.header.crc32, idx.last_key_ptr, idx.last_key_size);
//    }
//
//    // Recompute meta crc after payload finalized
//    meta_block.header.crc32 = ::crc32(0L, Z_NULL, 0);
//    compute_crc32(meta_block.header.crc32, &meta_block.payload, sizeof(meta_block.payload));
//}


//void SSTableManager::add_to_pool(MemTable& mem_table)
//{
//	this->immutable_pool.emplace_back(SSTable(mem_table, static_cast<uint32_t>(this->immutable_pool.size())));
//}