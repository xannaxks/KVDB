#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "arena.h"
#include "config.h"
#include "file.h"
#include "sstable.h"
#include "status.h"

struct WALFileHeader
{
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::uint32_t header_size = 0;

    std::uint64_t wal_id = 0;
    std::uint64_t start_seq = 0;

    std::uint32_t block_size = 0;
    std::uint32_t reserved = 0;

    std::uint32_t header_crc32 = 0;

    WALFileHeader() = default;
    WALFileHeader(std::uint32_t wal_id, std::uint64_t start_seq);

    [[nodiscard]] Status write(
        WritableFile& file,
        std::uint64_t& offset
    ) const;

    [[nodiscard]] static Result<std::optional<WALFileHeader>> load(
        ReadableFile& file,
        std::uint32_t expected_wal_id,
        std::uint64_t& offset
    );

    [[nodiscard]] bool self_check() const;
    [[nodiscard]] static constexpr std::uint32_t disk_size() noexcept
    {
        // u32 + u32 + u32 + u64 + u64 + u32 + u32 + u32
        return 40;
    }
    void compute_crc32();

    static std::uint32_t compute_crc32(const WALFileHeader& wal_file_header);
};

struct Fragment
{
    enum class Type : std::uint8_t
    {
        FULL = 0,
        FIRST = 1,
        MIDDLE = 2,
        LAST = 3,
    };

    struct Header
    {
        std::uint32_t fragment_crc32 = 0;
        std::uint32_t header_size = 0;

        Fragment::Type fragment_type = Fragment::Type::FULL;
        ::Type type = ::Type::Put;

        std::uint64_t seq_num = 0;
        std::uint32_t fragment_size = 0;

        [[nodiscard]] static constexpr std::uint32_t disk_size() noexcept
        {
            // u32 + u32 + u8 + u8 + u64 + u32
            return 22;
        }

        [[nodiscard]] Status write(
            WritableFile& file,
            std::uint64_t& offset
        ) const;

        [[nodiscard]] static Result<Header> load(
            ReadableFile& file,
            std::uint64_t& offset
        );
        // Fragment CRC also covers the payload, so it is computed by
        // Fragment::compute_crc32(), not by Header alone
        //[[nodiscard]] Status compute_crc32();
    };

    struct Payload
    {
        std::vector<std::byte> bytes;

        [[nodiscard]] Status write(
            WritableFile& file,
            std::uint64_t& offset
        );

        [[nodiscard]] static Result<Payload> load(
            ReadableFile& file,
            std::uint32_t size,
            std::uint64_t& offset
        );

        [[nodiscard]] std::size_t disk_size() const noexcept
        {
            return bytes.size();
        }
    };

    Header header;
    Payload payload;

    [[nodiscard]] Status compute_crc32();

    [[nodiscard]] static Status compute_crc32(
        std::uint32_t& crc32_out,
        const Fragment& fragment
    );

    [[nodiscard]] Status write(
        WritableFile& file,
        std::uint64_t& offset
    );

    [[nodiscard]] static Result<std::optional<Fragment>> load(
        ReadableFile& file,
        std::uint64_t& offset
    );

    [[nodiscard]] std::size_t disk_size() const noexcept
    {
        return Header::disk_size() + payload.disk_size();
    }
};

class WALWriter
{
public:
    WALWriter() = default;
    ~WALWriter();

    WALWriter(const WALWriter&) = delete;
    WALWriter& operator=(const WALWriter&) = delete;
    WALWriter(WALWriter&&) noexcept = default;
    WALWriter& operator=(WALWriter&&) noexcept = default;

    // Creates a fresh WAL. open_writable_file() truncates, so this must not
    // be used to reopen an existing WAL after recovery.
    [[nodiscard]] Status create(
        const std::filesystem::path& path,
        std::uint32_t wal_id,
        std::uint64_t start_seq
    );

    [[nodiscard]] Status rotate(
        const std::filesystem::path& new_path,
        std::uint32_t new_wal_id,
        std::uint64_t start_seq
    );

    [[nodiscard]] Status write(const InternalRecord& record);
    [[nodiscard]] Status sync();
    [[nodiscard]] Status close();

    [[nodiscard]] bool is_open() const noexcept
    {
        return file_ != nullptr;
    }

    [[nodiscard]] std::uint32_t wal_id() const noexcept
    {
        return wal_id_;
    }

    [[nodiscard]] std::uint64_t offset() const noexcept
    {
        return offset_;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    [[nodiscard]] Status write_payload(
        std::span<const std::byte> byte_sequence,
        const InternalRecord& record
    );

    std::unique_ptr<WritableFile> file_;
    std::filesystem::path path_;
    std::uint32_t wal_id_ = 0;
    std::uint64_t offset_ = 0;
};

class WALLoader
{
public:
    struct LoadResult
    {
        // key/value bytes are owned by the Arena passed to load().
        std::vector<InternalRecord> records;
        std::optional<WALFileHeader> header;

        // Safe truncation/recovery boundary: immediately after the last
        // completely reconstructed logical record, or after the file header.

        bool ok = true;
        bool had_torn_tail = false;
        bool had_corruption = false;
        std::string error;
    };

    [[nodiscard]] static Result<LoadResult> load(
        ReadableFile& file,
        std::uint64_t& offset,
        std::uint32_t expected_wal_id,
        Arena& arena
    );

    [[nodiscard]] static Result<LoadResult> load(
        const std::filesystem::path& path,
        std::uint32_t expected_wal_id,
        Arena& arena
    );
};

class WALStreamingLoader
{
public:
    struct LoadResult
    {
        // Present only when the latest load_next() produced a record.
        std::optional<InternalRecord> logical_record;

        // Persists after the WAL header has been loaded.
        std::optional<WALFileHeader> header;

        // Describes the latest load_next() call.
        bool reached_eof = false;
        bool ok = true;
        bool had_torn_tail = false;
        bool had_corruption = false;

        // End offset of the last completely valid item.
        // Useful when truncating a torn WAL tail.
        std::uint64_t last_good_offset = 0;

        std::string error;
    };

    WALStreamingLoader(
        std::filesystem::path path,
        Arena& arena
    )
        : path_(std::move(path)),
        arena_(arena)
    {
    }

    WALStreamingLoader() = delete;

    WALStreamingLoader(const WALStreamingLoader&) = delete;
    WALStreamingLoader& operator=(const WALStreamingLoader&) = delete;

    [[nodiscard]] Status open();

    [[nodiscard]] Status load_next(
        std::uint64_t& offset,
        std::uint32_t expected_wal_id
    );

    [[nodiscard]] const LoadResult& result() const noexcept
    {
        return result_;
    }

private:
    enum class State
    {
        Closed,
        Ready,
        EndOfFile,
        TornTail,
        Corrupted
    };

    [[nodiscard]] Status validate_state(
        std::uint64_t offset,
        std::uint32_t expected_wal_id
    ) const;

    void reset_call_result();

    void mark_torn_tail(std::string message);

    void mark_corruption(std::string message);

private:
    std::filesystem::path path_;
    std::unique_ptr<ReadableFile> file_;
    Arena& arena_;

    LoadResult result_;

    State state_ = State::Closed;

    std::string terminal_error_;
};

class WAL
{
public:
    WAL() = default;

    [[nodiscard]] Status create(
        const std::filesystem::path& path,
        std::uint32_t wal_id,
        std::uint64_t start_seq
    )
    {
        return writer_.create(path, wal_id, start_seq);
    }

    [[nodiscard]] Status rotate(
        const std::filesystem::path& new_path,
        std::uint32_t new_wal_id,
        std::uint64_t start_seq
    )
    {
        return writer_.rotate(new_path, new_wal_id, start_seq);
    }

    [[nodiscard]] Status write(const InternalRecord& record)
    {
        return writer_.write(record);
    }

    [[nodiscard]] Status sync()
    {
        return writer_.sync();
    }

    [[nodiscard]] Status close()
    {
        return writer_.close();
    }

    [[nodiscard]] static Result<WALLoader::LoadResult> recover(
        const std::filesystem::path& path,
        std::uint32_t expected_wal_id,
        Arena& arena
    )
    {
        return WALLoader::load(path, expected_wal_id, arena);
    }

    [[nodiscard]] WALWriter& writer() noexcept
    {
        return writer_;
    }

    [[nodiscard]] const WALWriter& writer() const noexcept
    {
        return writer_;
    }

private:
    WALWriter writer_;
};
