/**
 * @file manifest.h
 * @brief Append-only version log for durable database metadata.
 *
 * The manifest records VersionEdit entries rather than rewriting the complete
 * level state. Recovery replays the valid prefix transactionally into a
 * LevelManager. A torn final edit is recoverable; corruption within the durable
 * prefix is reported as an error.
 */
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
inline constexpr std::uint32_t MANIFEST_MAX_EDIT_SIZE = 64u * 1024u * 1024u;
inline constexpr std::uint32_t MANIFEST_MAX_TABLES_PER_EDIT = 1'000'000u;

/** @brief Compact manifest record identifying one table removed from a level. */
struct DeletedTable
{
    std::uint32_t level = 0;
    std::uint64_t table_id = 0;

    static Result<DeletedTable> load(
        ReadableFile& file,
        std::uint64_t& offset
    );

    Status write(
        WritableFile& file,
        std::uint64_t& offset
    ) const;

    static constexpr std::uint32_t disk_size() noexcept
    {
        return 4u + 8u;
    }

    void calculate_crc(
        std::uint32_t& crc_buffer,
        bool init = false
    ) const;
};

/**
 * @brief Atomic metadata transition stored in the manifest.
 *
 * Optional counters advance allocator/recovery state. Table additions and
 * deletions describe one flush or compaction publication. prepare() validates
 * the payload and seals its size/checksum before write().
 */
struct VersionEdit
{
    struct Header
    {
        std::uint32_t crc32 = 0;
        std::uint32_t payload_size = 0;

        static constexpr std::uint32_t disk_size() noexcept
        {
            return 4u + 4u;
        }

        Status write(
            WritableFile& file,
            std::uint64_t& offset
        ) const;

        static Result<Header> load(
            ReadableFile& file,
            std::uint64_t& offset
        );
    };

    struct Payload
    {
        std::optional<std::uint64_t> next_table_id;
        std::optional<std::uint64_t> next_sequence_number;
        std::optional<std::uint64_t> current_wal_id;

        std::vector<DeletedTable> deleted_tables;
        std::vector<TableMeta> new_tables;

        [[nodiscard]] Result<std::uint32_t> encoded_size() const;

        Status write(
            WritableFile& file,
            std::uint64_t& offset
        ) const;

        static Result<Payload> load(
            ReadableFile& file,
            std::uint64_t& offset,
            std::uint32_t payload_size,
            Arena& arena
        );

        Status compute_crc32(std::uint32_t& crc_buffer) const;
        Status validate() const;
    };

    Header header{};
    Payload payload{};

    [[nodiscard]] Result<std::uint32_t> encoded_size() const;

    Status add_table(const TableMeta& meta);
    /** @brief Validates the payload and rebuilds its encoded header. */
    Status prepare();

    Status write(
        WritableFile& file,
        std::uint64_t& offset
    );

    /** @brief Loads one bounded, checksum-verified edit at @p offset. */
    static Result<VersionEdit> load(
        ReadableFile& file,
        std::uint64_t& offset,
        std::uint64_t file_size,
        Arena& arena
    );
};

/**
 * @brief Durable owner of LSM version counters and table metadata changes.
 *
 * Runtime commits are staged against a copy of LevelManager, appended and
 * synchronized, then published in memory. This ordering prevents failed
 * validation or I/O from exposing a partial metadata transition.
 *
 * After an append/sync failure the instance is write-poisoned and must be
 * recovered from disk before more metadata can be committed.
 */
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

        static constexpr std::uint32_t disk_size() noexcept
        {
            return 6u * 4u;
        }

        Status compute_crc32();

        Status write(
            WritableFile& file,
            std::uint64_t& offset
        ) const;

        static Result<Header> load(
            ReadableFile& file,
            std::uint64_t& offset
        );
    };

    Manifest() = default;
    explicit Manifest(std::filesystem::path path);

    /**
     * @brief Replays the valid manifest prefix into @p level_manager.
     *
     * Recovery is transactional with respect to the supplied manager: on
     * failure, the caller's state remains unchanged.
     *
     * @callgraph
     */
    static Result<Manifest> load(
        LevelManager& level_manager,
        const std::filesystem::path& path,
        Arena& arena
    );

    /** @brief Opens an existing manifest or creates and syncs a new header. */
    Status open_or_create();

    /**
     * @brief Converts recovered read state into a safe appendable state.
     *
     * A loaded manifest deliberately has no writer attached. This operation
     * closes the reader and truncates a recoverable torn tail before attaching
     * a writer positioned at append_offset().
     */
    Status prepare_for_append();
    Status attach_writer(std::unique_ptr<WritableFile> writable);

    /** @brief Applies an edit atomically in memory without durable append. */
    Status apply(LevelManager& level_manager, const VersionEdit& edit);

    /**
     * @brief Durably appends an edit, then publishes its staged metadata state.
     * @warning A write or sync failure poisons this object; reload it.
     * @callgraph
     */
    Status commit(LevelManager& level_manager, VersionEdit& edit);

    Status sync();

    [[nodiscard]] Result<std::uint64_t> allocate_table_id();

    [[nodiscard]] std::uint64_t current_wal_id() const noexcept;
    [[nodiscard]] std::uint64_t next_sequence_number() const noexcept;
    [[nodiscard]] std::uint64_t next_table_id() const noexcept;
    [[nodiscard]] std::uint64_t append_offset() const noexcept;
    [[nodiscard]] bool has_recoverable_tail() const noexcept;
    [[nodiscard]] bool write_poisoned() const noexcept;

private:
    struct StagedCounters
    {
        std::uint64_t next_table_id = 1;
        std::uint64_t current_wal_id = 1;
        std::uint64_t next_sequence_number = 1;
    };

    Status append_prepared(VersionEdit& edit);

    static Status stage_apply(
        LevelManager& level_manager,
        StagedCounters& counters,
        const VersionEdit& edit
    );

    static Status check_invariants(
        const LevelManager& level_manager,
        const StagedCounters& counters
    );

    [[nodiscard]] StagedCounters counters() const noexcept;
    void publish_counters(const StagedCounters& counters) noexcept;

private:
    std::filesystem::path path_;
    Header header_{};

    std::uint64_t next_table_id_ = 1;
    std::uint64_t current_wal_id_ = 1;
    std::uint64_t next_sequence_number_ = 1;

    std::unique_ptr<WritableFile> writable_;
    std::unique_ptr<ReadableFile> readable_;
    std::uint64_t append_offset_ = 0;

    bool recoverable_tail_ = false;
    bool write_poisoned_ = false;
};
