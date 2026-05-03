//#include "manifest.h"
//
//uint32_t Manifest::Header::disk_size()
//{
//	return (
//		sizeof(magic) + 
//		sizeof(version) + 
//		sizeof(block_size) +
//		sizeof(header_size) + 
//		sizeof(reserved) + 
//		sizeof(crc32)
//	);
//}
//
//uint32_t Manifest::Header::compute_crc32()
//{
//	this->crc32 = ::crc32(0L, Z_NULL, 0);
//	crc32_add_pod(this->crc32, this->magic);
//	crc32_add_pod(this->crc32, this->version);
//	crc32_add_pod(this->crc32, this->block_size);
//	crc32_add_pod(this->crc32, this->header_size);
//	crc32_add_pod(this->crc32, this->reserved);
//}
//
//void Manifest::Header::write(std::ofstream& file)
//{
//	align_to_block_boundary(file, this->block_size);
//	file.write(reinterpret_cast<const char*>(&this->magic), static_cast<std::streamsize>(sizeof(this->magic)));
//	file.write(reinterpret_cast<const char*>(&this->version), static_cast<std::streamsize>(sizeof(this->version)));
//	file.write(reinterpret_cast<const char*>(&this->block_size), static_cast<std::streamsize>(sizeof(this->block_size)));
//	file.write(reinterpret_cast<const char*>(&this->header_size), static_cast<std::streamsize>(sizeof(this->header_size)));
//	file.write(reinterpret_cast<const char*>(&this->reserved), static_cast<std::streamsize>(sizeof(this->reserved)));
//	file.write(reinterpret_cast<const char*>(&this->crc32), static_cast<std::streamsize>(sizeof(this->crc32)));
//}
//
//void VersionEdit::Header::compute_crc32(VersionEdit& version_edit)
//{
//	this->crc32 = ::crc32(0L, Z_NULL, 0);
//	crc32_add_pod(this->crc32, version_edit.next_table_id);
//	crc32_add_pod(this->crc32, version_edit.last_seq_num);
//	crc32_add_pod(this->crc32, version_edit.current_wal_id);
//
//	for (auto& new_table : version_edit.new_tables)
//		new_table.compute_crc32(this->crc32);
//
//	for (auto& deleted_table : version_edit.deleted_tables)
//		deleted_tables.compute_crc32(this->crc32);
//}
//void VersionEdit::Header::compute_payload_size(VersionEdit& version_edit)
//{
//	this->payload_size = version_edit.disk_size();
//}

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

inline constexpr uint32_t MANIFEST_MAGIC = 0x4D414E49; // "MANI"
inline constexpr uint32_t MANIFEST_VERSION = 1;
inline constexpr uint32_t MANIFEST_BLOCK_SIZE = 4096;
inline constexpr uint32_t DEFAULT_LEVEL_COUNT = 7;

struct DeletedTable
{
    uint32_t level = 0;
    uint64_t table_id = 0;
};

struct SSTableMeta
{
    uint64_t table_id = 0;
    uint64_t file_size = 0;

    uint64_t smallest_seq = 0;
    uint64_t largest_seq = 0;

    std::vector<std::byte> smallest_key;
    std::vector<std::byte> largest_key;

    uint64_t record_count = 0;
    uint64_t tombstone_count = 0;

    uint32_t level = 0;
};

struct VersionEdit
{
    std::optional<uint64_t> next_table_id;
    std::optional<uint64_t> last_sequence_number;
    std::optional<uint64_t> current_wal_id;

    std::vector<SSTableMeta> added_tables;
    std::vector<DeletedTable> deleted_tables;
};

class Manifest
{
public:
    struct Header
    {
        uint32_t magic = MANIFEST_MAGIC;
        uint32_t version = MANIFEST_VERSION;
        uint32_t block_size = MANIFEST_BLOCK_SIZE;
        uint32_t header_size = disk_size();
        uint32_t reserved = 0;
        uint32_t crc32 = 0;

        static constexpr uint32_t disk_size()
        {
            return sizeof(uint32_t) * 6;
        }

        void compute_crc32();
        bool is_valid() const;

        void write(std::ofstream& file) const;
        static std::optional<Header> load(std::ifstream& file);
    };

public:
    explicit Manifest(uint32_t level_count = DEFAULT_LEVEL_COUNT);

    uint64_t allocate_table_id();

    uint64_t next_table_id() const;
    uint64_t last_sequence_number() const;
    uint64_t current_wal_id() const;

    const std::vector<SSTableMeta>& level(uint32_t level) const;
    const std::vector<std::vector<SSTableMeta>>& levels() const;

    bool apply(const VersionEdit& edit);

    static bool create_new(const std::filesystem::path& path, const VersionEdit& initial_edit);
    static bool append_edit(const std::filesystem::path& path, const VersionEdit& edit);

    static std::optional<Manifest> load_from_file(
        const std::filesystem::path& path,
        uint32_t level_count = DEFAULT_LEVEL_COUNT
    );

private:
    static bool valid_table_meta(const SSTableMeta& table);
    bool check_invariants() const;

private:
    uint64_t next_table_id_ = 1;
    uint64_t last_sequence_number_ = 0;
    uint64_t current_wal_id_ = 1;

    std::vector<std::vector<SSTableMeta>> levels_;
};

namespace manifest_codec
{
    bool write_record(std::ofstream& file, const VersionEdit& edit);
    std::optional<VersionEdit> read_record(std::ifstream& file);
}