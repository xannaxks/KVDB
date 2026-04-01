#include "sstable.h"

using namespace SSTableEntities;

/* =========================
   Data
   ========================= */

Data::Data(
    uint32_t key_size,
    const Bytes& key,
    uint32_t value_size,
    const Bytes& value,
    Type type,
    uint64_t seq_num
)
    : key_size(key_size),
    value_size(value_size),
    type(type),
    seq_num(seq_num),
    key(key),
    value(value)
{
}
Data::Data(const InternalRecord& record)
    : key_size(static_cast<uint32_t>(record.key.size())),
    value_size(static_cast<uint32_t>(record.value.size())),
    type(record.type),
    seq_num(record.seq_num),
    key(record.key),
    value(record.value)
{
}

uint32_t Data::get_size() const
{
    return static_cast<uint32_t>(
        sizeof(key_size) +
        sizeof(value_size) +
        sizeof(type) +
        sizeof(seq_num) +
        key_size +
        value_size
        );
}

void Data::write(std::ofstream& file, int& in_block_offset, uint32_t& block_offset) const
{
    ensure_block(file, in_block_offset, block_offset, get_size());

    file.write(reinterpret_cast<const char*>(&key_size), sizeof(key_size));
    if (!file) throw std::runtime_error("Failed to write sstable data: key_size");

    file.write(reinterpret_cast<const char*>(&value_size), sizeof(value_size));
    if (!file) throw std::runtime_error("Failed to write sstable data: value_size");

    file.write(reinterpret_cast<const char*>(&type), sizeof(type));
    if (!file) throw std::runtime_error("Failed to write sstable data: type");

    file.write(reinterpret_cast<const char*>(&seq_num), sizeof(seq_num));
    if (!file) throw std::runtime_error("Failed to write sstable data: seq_num");

    file.write(reinterpret_cast<const char*>(key.data()), static_cast<std::streamsize>(key_size));
    if (!file) throw std::runtime_error("Failed to write sstable data: key");

    file.write(reinterpret_cast<const char*>(value.data()), static_cast<std::streamsize>(value_size));
    if (!file) throw std::runtime_error("Failed to write sstable data: value");

    in_block_offset += static_cast<int>(get_size());
}
Data Data::read(std::ifstream& file)
{
    Data data;

    file.read(reinterpret_cast<char*>(&data.key_size), sizeof(data.key_size));
    if (!file)
        throw std::runtime_error("Failed to read sstable data: key_size");

    file.read(reinterpret_cast<char*>(&data.value_size), sizeof(data.value_size));
    if (!file)
        throw std::runtime_error("Failed to read sstable data: value_size");

    file.read(reinterpret_cast<char*>(&data.type), sizeof(data.type));
    if (!file)
        throw std::runtime_error("Failed to read sstable data: type");

    file.read(reinterpret_cast<char*>(&data.seq_num), sizeof(data.seq_num));
    if (!file)
        throw std::runtime_error("Failed to read sstable data: seq_num");

    if (data.key_size > SSTableEntities::BLOCK_SIZE)
        throw std::runtime_error("Failed to read sstable data: key size corrupted");
    if (data.value_size > SSTableEntities::BLOCK_SIZE)
        throw std::runtime_error("Failed to read sstable data: value size corrupted");

    data.key.resize(data.key_size);
    data.value.resize(data.value_size);

    file.read(reinterpret_cast<char*>(data.key.data()), static_cast<std::streamsize>(data.key_size));
    if (!file)
        throw std::runtime_error("Failed to read sstable data: key");

    file.read(reinterpret_cast<char*>(data.value.data()), static_cast<std::streamsize>(data.value_size));
    if (!file)
        throw std::runtime_error("Failed to read sstable data: value");

    if (data.key_size != data.key.size())
        throw std::runtime_error("Failed to read sstable: key size doesn't correspond");
    if (data.value_size != data.value.size())
        throw std::runtime_error("Failed to read sstable: value size doesn't correpond");

    return data;
}

/* =========================
   Index
   ========================= */

Index::Index(uint32_t key_size, const Bytes& key, uint32_t block_offset, uint32_t data_coverage)
    : key_size(key_size),
    block_offset(block_offset),
    data_coverage(data_coverage),
    key(key)
{
}
Index::Index(const Data& data, uint32_t block_offset, uint32_t data_coverage)
    : key_size(data.key_size),
    block_offset(block_offset),
    data_coverage(data_coverage),
    key(data.key)
{
}

uint32_t Index::get_size() const
{
    return static_cast<uint32_t>(
        sizeof(key_size) +
        sizeof(block_offset) +
        sizeof(data_coverage) +
        key_size
        );
}

void Index::write(std::ofstream& file, int& in_block_offset, uint32_t& block_offset) const
{
    ensure_block(file, in_block_offset, block_offset, get_size());

    file.write(reinterpret_cast<const char*>(&key_size), sizeof(key_size));
    if (!file) throw std::runtime_error("Failed to write sstable index: key_size");

    file.write(reinterpret_cast<const char*>(&block_offset), sizeof(block_offset));
    if (!file) throw std::runtime_error("Failed to write sstable index: block_offset");

    file.write(reinterpret_cast<const char*>(&data_coverage), sizeof(data_coverage));
    if (!file) throw std::runtime_error("Failed to write sstable index: data_coverage");

    file.write(reinterpret_cast<const char*>(key.data()), static_cast<std::streamsize>(key_size));
    if (!file) throw std::runtime_error("Failed to write sstable index: key");

    in_block_offset += static_cast<int>(get_size());
}
Index Index::read(std::ifstream& file)
{
    Index index;

    file.read(reinterpret_cast<char*>(&index.key_size), sizeof(index.key_size));
    if (!file)
        throw std::runtime_error("Failed to read sstable index: key_size");

    file.read(reinterpret_cast<char*>(&index.block_offset), sizeof(index.block_offset));
    if (!file)
        throw std::runtime_error("Failed to read sstable index: block_offset");

    file.read(reinterpret_cast<char*>(&index.data_coverage), sizeof(index.data_coverage));
    if (!file)
        throw std::runtime_error("Failed to read sstable index: data_coverage");

    index.key.resize(index.key_size);
    file.read(reinterpret_cast<char*>(index.key.data()), static_cast<std::streamsize>(index.key_size));
    if (!file)
        throw std::runtime_error("Failed to read sstable index: key");

    return index;
}

/* =========================
   Footer
   ========================= */

Footer::Footer(int in_block_offset, uint32_t block_offset, uint32_t index_count)
    : magic(SSTableMagic),
    in_block_offset(in_block_offset),
    block_offset(block_offset),
    index_count(index_count)
{
}

uint32_t Footer::get_size() const
{
    return static_cast<uint32_t>(
        sizeof(magic) +
        sizeof(in_block_offset) +
        sizeof(block_offset) +
        sizeof(index_count)
        );
}

void Footer::write(std::ofstream& file, int& in_block_offset, uint32_t& block_offset) const
{
    ensure_block(file, in_block_offset, block_offset, get_size());

    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    if (!file) throw std::runtime_error("Failed to write sstable footer: magic");

    file.write(reinterpret_cast<const char*>(&in_block_offset), sizeof(in_block_offset));
    if (!file) throw std::runtime_error("Failed to write sstable footer: in_block_offset");

    file.write(reinterpret_cast<const char*>(&block_offset), sizeof(block_offset));
    if (!file) throw std::runtime_error("Failed to write sstable footer: block_offset");

    file.write(reinterpret_cast<const char*>(&index_count), sizeof(index_count));
    if (!file) throw std::runtime_error("Failed to write sstable footer: index_count");
}
Footer Footer::read(std::ifstream& file)
{
    Footer footer;

    file.read(reinterpret_cast<char*>(&footer.magic), sizeof(footer.magic));
    if (!file)
        throw std::runtime_error("Failed to read sstable footer: magic");

    if (footer.magic != SSTableMagic)
        throw std::runtime_error("Corrupted sstable: footer magic mismatch");

    file.read(reinterpret_cast<char*>(&footer.in_block_offset), sizeof(footer.in_block_offset));
    if (!file)
        throw std::runtime_error("Failed to read sstable footer: in_block_offset");

    file.read(reinterpret_cast<char*>(&footer.block_offset), sizeof(footer.block_offset));
    if (!file)
        throw std::runtime_error("Failed to read sstable footer: block_offset");

    file.read(reinterpret_cast<char*>(&footer.index_count), sizeof(footer.index_count));
    if (!file)
        throw std::runtime_error("Failed to read sstable footer: index_count");

    return footer;
}

/* =========================
   Block helpers
   ========================= */

void SSTableEntities::next_block(std::ofstream& file, int& in_block_offset, uint32_t& block_offset)
{
    static const char zeros[BLOCK_SIZE] = {};
    const int remaining = BLOCK_SIZE - in_block_offset;

    file.write(zeros, static_cast<std::streamsize>(remaining));
    if (!file)
        throw std::runtime_error("Failed to advance to next sstable block");

    ++block_offset;
    in_block_offset = 0;
}
void SSTableEntities::ensure_block(std::ofstream& file, int& in_block_offset, uint32_t& block_offset, uint32_t size)
{
    if (size > static_cast<uint32_t>(BLOCK_SIZE))
        throw std::runtime_error("SSTable record is larger than block size");

    if (in_block_offset + static_cast<int>(size) > BLOCK_SIZE)
        next_block(file, in_block_offset, block_offset);
}
std::string SSTableEntities::construct_sstable_name(const std::string& sstable_dir, uint32_t sstable_num)
{
    return std::format("{}/{:05}.sst", sstable_dir, sstable_num);
}

/* =========================
   Builder
   ========================= */

void SSTableBuilder::build_from_records(const std::vector<InternalRecord>& records, const std::string& path)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
        throw std::runtime_error("Failed to open/create sstable file");

    uint32_t block_offset = 0;
    int in_block_offset = 0;
    std::vector<Index> indexes;

    bool block_has_data = false;
    uint32_t current_block_offset = 0;
    Bytes first_key_of_block;
    uint32_t current_block_coverage = 0;
    const InternalRecord* prev_record = nullptr;
    for (const auto& record : records)
    {
        if (prev_record != nullptr && record.key == prev_record->key) continue;
        Data data(record);

        if (data.get_size() > SSTableEntities::BLOCK_SIZE)
            throw std::runtime_error("Failed to write data in sstable: larger than block size");

        // If record doesn't fit, finish previous block first.
        if (in_block_offset + static_cast<int>(data.get_size()) > BLOCK_SIZE)
        {
            if (block_has_data)
            {
                indexes.emplace_back(
                    static_cast<uint32_t>(first_key_of_block.size()),
                    first_key_of_block,
                    current_block_offset,
                    current_block_coverage
                );
            }

            next_block(file, in_block_offset, block_offset);
            block_has_data = false;
            current_block_coverage = 0;
        }

        // New block starts here
        if (!block_has_data)
        {
            block_has_data = true;
            current_block_offset = block_offset;
            first_key_of_block = data.key;
            current_block_coverage = 0;
        }

        data.write(file, in_block_offset, block_offset);
        current_block_coverage += data.get_size();
        prev_record = &record;
    }

    // Final block
    if (block_has_data)
    {
        indexes.emplace_back(
            static_cast<uint32_t>(first_key_of_block.size()),
            first_key_of_block,
            current_block_offset,
            current_block_coverage
        );
    }

    const uint32_t index_block_offset = block_offset;
    const int index_in_block_offset = in_block_offset;

    for (const auto& index : indexes)
        index.write(file, in_block_offset, block_offset);

    Footer footer(index_in_block_offset, index_block_offset,
        static_cast<uint32_t>(indexes.size()));
    footer.write(file, in_block_offset, block_offset);
}
void SSTableBuilder::build_from_memtable(MemTable& mem_table, const std::string& sstable_dir, uint32_t& sstable_num)
{
    while (mem_table.has_immutable())
    {
        std::vector<InternalRecord> out;
        mem_table.dump_oldest_immutable(out);

        build_from_records(out, construct_sstable_name(sstable_dir, sstable_num));

        mem_table.drop_oldest_immutable();
        ++sstable_num;
    }
}

/* =========================
   SSTable reader
   ========================= */

SSTable::SSTable(const std::string& path)
    : path(path), file(path, std::ios::binary)
{
    if (!file)
        throw std::runtime_error("Failed to open sstable");

    load_footer();
    load_indexes();

    if (this->footer.index_count != this->indexes.size())
        throw std::runtime_error("Failed to open sstable: indexes count doesn't correspond");
}

void SSTable::load_footer()
{
    Footer temp;
    file.seekg(-static_cast<std::streamoff>(temp.get_size()), std::ios::end);
    footer = Footer::read(file);
}
void SSTable::load_indexes()
{
    file.seekg(
        static_cast<std::streamoff>(footer.block_offset) * BLOCK_SIZE +
        static_cast<std::streamoff>(footer.in_block_offset),
        std::ios::beg
    );

    indexes.clear();
    indexes.reserve(footer.index_count);

    for (uint32_t i = 0; i < footer.index_count; ++i)
        indexes.emplace_back(Index::read(file));
}

std::variant<ByteRecord, SSTable::Status> SSTable::get(const Bytes& key)
{
    auto it = std::upper_bound(
        indexes.begin(),
        indexes.end(),
        key,
        [](const Bytes& key, const Index& node)
        {
            return key < node.key;
        }
    );

    if (it == indexes.begin())
        return Status::KeyNotFound;

    --it;

    file.seekg(
        static_cast<std::streamoff>(it->block_offset) * BLOCK_SIZE,
        std::ios::beg
    );

    std::vector<Data> datas;
    datas.reserve(16);

    uint32_t bytes_read = 0;
    while (bytes_read < it->data_coverage)
    {
        Data data = Data::read(file);
        bytes_read += data.get_size();
        datas.emplace_back(std::move(data));
    }
    if (bytes_read != it->data_coverage)
        throw std::runtime_error("sstable corrupted: failed to load data");
    if (bytes_read > SSTableEntities::BLOCK_SIZE)
        throw std::runtime_error("sstable corrupted: blocks size big");

    auto result = std::lower_bound(
        datas.begin(),
        datas.end(),
        key,
        [](const Data& data, const Bytes& key)
        {
            return data.key < key;
        }
    );

    if (result == datas.end() || result->key != key)
        return Status::KeyNotFound;

    if (result->type == Type::Tombstone)
        return SSTable::Status::KeyWasDeleted;

    return ByteRecord(result->key, result->value, result->type);
}


/* ========================
   SSTable Bloom Filters
   ======================== */

SSTableEntities::SSTableBloomFilter::SSTableBloomFilter()
{
    this->bloom_ptr = std::make_unique<uint8_t[]>(SSTableBloomFilter::get_storage_size());
    std::fill_n(bloom_ptr.get(), get_storage_size(), 0);
}
std::vector<uint32_t> SSTableEntities::SSTableBloomFilter::get_hashes(const Bytes& key, const int hash_amount) const
{
    std::vector<uint32_t> hashes;
    uint32_t h1 = get_hash1(key);
    uint32_t h2 = get_hash2(key);
    if (h2 == 0) h2 = 1;

    hashes.reserve(hash_amount);
    for (int i = 0; i < hash_amount; i++)
    {
        hashes.push_back((h1 + i * h2) % SSTableEntities::SSTableBloomFilter::get_size());
    }
    return hashes;
}
void SSTableEntities::SSTableBloomFilter::add_key(const Bytes& key)
{
    auto hashes = get_hashes(key);
    for (auto hash : hashes)
        set_bit(hash);
}
uint32_t SSTableEntities::SSTableBloomFilter::get_storage_size()
{
    return (SSTableEntities::SSTableBloomFilter::bloom_bits + 7) / 8;
}
SSTableEntities::SSTableBloomFilter SSTableEntities::SSTableBloomFilter::read(std::ifstream& file)
{
    if (!file)
        throw std::runtime_error("Failed to read bloom filter in sstable: file failed");
    SSTableBloomFilter bloom;
    auto size = SSTableBloomFilter::get_storage_size();
    file.read(reinterpret_cast<char*>(bloom.bloom_ptr.get()), static_cast<std::streamsize>(size));
    return bloom;
}
void SSTableEntities::SSTableBloomFilter::write(std::ofstream& file, int& in_block_offset, uint32_t& block_offset) const
{
    auto size = SSTableBloomFilter::get_storage_size();
    file.write(reinterpret_cast<const char*>(this->bloom_ptr.get()), static_cast<std::streamsize>(size));
    if (!file)
        throw std::runtime_error("Failed to write bloom filter in sstable");
    in_block_offset += size;
    block_offset += in_block_offset / SSTableEntities::BLOCK_SIZE;
    in_block_offset %= SSTableEntities::BLOCK_SIZE;
}
void SSTableEntities::SSTableBloomFilter::set_bit(uint32_t bit_index)
{
    bloom_ptr[bit_index / 8] |= static_cast<uint8_t>(1u << (bit_index % 8));
}
bool SSTableEntities::SSTableBloomFilter::test_bit(uint32_t bit_index) const
{
    return (bloom_ptr[bit_index / 8] & static_cast<uint8_t>(1u << (bit_index % 8))) != 0;
}
std::vector<uint32_t> SSTableEntities::SSTableBloomFilter::get_hashes(const Bytes& key, int hash_amount) const
{
    std::vector<uint32_t> hashes;
    hashes.reserve(hash_amount);
    uint32_t hash1 = get_hash1(key);
    uint32_t hash2 = get_hash2(key);
    if (hash2 == 0) hash2 = 1;
    
    for (int i = 0; i < hash_amount; i++)
        hashes.push_back((hash1 + (i * hash2)) % bloom_bits);
    
    return hashes;
}
bool SSTableEntities::SSTableBloomFilter::may_contain(const Bytes& key) const
{
    auto hashes = get_hashes(key);
    for (auto hash : hashes)
        if (!test_bit(hash))
            return false;
    return true;
}
uint32_t SSTableEntities::SSTableBloomFilter::get_hash1(const Bytes& key)
{
    uint32_t out = 0;
    MurmurHash3_x86_32(key.data(), static_cast<int>(key.size()), SSTableEntities::seed1, &out);
    return out;
}
uint32_t SSTableEntities::SSTableBloomFilter::get_hash2(const Bytes& key)
{
    uint32_t out = 0;
    MurmurHash3_x86_32(key.data(), static_cast<int>(key.size()), SSTableEntities::seed2, &out);
    return out;
}