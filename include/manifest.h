#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>
#include "arena.h"
#include "file.h"
#include "level_manager.h"
#include "status.h"
#include "table_meta.h"

inline constexpr std::uint32_t MANIFEST_MAGIC = 0x4D414E49; // "MANI"
inline constexpr std::uint32_t MANIFEST_VERSION = 1;

struct DeletedTable
{
    std::uint32_t level = 0;
    std::uint64_t table_id = 0;

    static Result<DeletedTable> load(ReadableFile& file, std::uint64_t& offset);
    Status write(WritableFile& file, std::uint64_t& offset) const;

    static constexpr std::uint32_t disk_size()
    {
        return sizeof(level) + sizeof(table_id);
    }

    void calculate_crc(std::uint32_t& crc_buffer, bool init = false) const;
};

struct VersionEdit
{
    struct Header
    {
        std::uint32_t crc32 = 0;
        std::uint32_t payload_size = 0;

        static constexpr std::uint32_t disk_size()
        {
            return sizeof(crc32) + sizeof(payload_size);
        }

        Status write(WritableFile& file, std::uint64_t& offset) const;
        static Result<Header> load(ReadableFile& file, std::uint64_t& offset);
    };

    struct Payload
    {
        std::optional<std::uint64_t> next_table_id;
        std::optional<std::uint64_t> next_sequence_number;
        std::optional<std::uint64_t> current_wal_id;

        std::vector<DeletedTable> deleted_tables;
        std::vector<TableMeta> new_tables;

        std::uint32_t disk_size() const;

        Status write(WritableFile& file, std::uint64_t& offset) const;
        static Result<Payload> load(
            ReadableFile& file,
            std::uint64_t& offset,
            std::uint32_t payload_size,
            Arena& arena
        );

        void compute_crc32(std::uint32_t& crc_buffer) const;
    };

    Header header{};
    Payload payload{};

    std::uint32_t disk_size() const
    {
        return Header::disk_size() + payload.disk_size();
    }

    Status add_table(const TableMeta& meta);
    Status write(WritableFile& file, std::uint64_t& offset);
    static Result<VersionEdit> load(ReadableFile& file, std::uint64_t& offset, Arena& arena);
};

class Manifest
{
public:
    struct Header
    {
        std::uint32_t magic = MANIFEST_MAGIC;
        std::uint32_t version = MANIFEST_VERSION;
        std::uint32_t header_size = Header::disk_size();
        std::uint32_t flags = 0;
        std::uint32_t reserved = 0;
        std::uint32_t crc32 = 0;

        static constexpr std::uint32_t disk_size()
        {
            return sizeof(std::uint32_t) * 6;
        }

        Status compute_crc32();

        Status write(WritableFile& file, std::uint64_t& offset) const;
        static Result<Header> load(ReadableFile& file, std::uint64_t& offset);
    };

private:
    std::filesystem::path path_;

    Header header_{};
    LevelManager level_manager_;

    std::uint64_t next_table_id_ = 1;
    std::uint64_t current_wal_id_ = 1;
    std::uint64_t next_sequence_number_ = 1;

    std::unique_ptr<WritableFile> writable_;
    std::unique_ptr<ReadableFile> readable_;
    std::uint64_t append_offset_ = 0;

public:
    Manifest() = default;
    explicit Manifest(std::filesystem::path path);

    static Result<Manifest> load(const std::filesystem::path& path, Arena& arena);
    Status open_or_create();

    Status append(VersionEdit& edit);
    Status apply(const VersionEdit& edit);
    Status commit(VersionEdit& edit);

    Status sync();

    std::uint64_t allocate_table_id();

    std::uint64_t current_wal_id() const;
    std::uint64_t next_sequence_number() const;
    std::uint64_t next_table_id() const;

    const LevelManager& level_manager() const;
    LevelManager& mutable_level_manager();

private:
    Status check_invariants() const;
};
