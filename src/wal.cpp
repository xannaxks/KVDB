#include "wal.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <format>
#include <new>
#include <string>
#include <utility>

#include "crc32_helpers.h"
#include "endian_io.h"
#include "file_helpers.h"

void WALFileHeader::compute_crc32()
{
    this->header_crc32 = ::crc32(0, Z_NULL, NULL);
   

    ::crc32_add_pod<std::uint32_t>(this->header_crc32, magic);
    ::crc32_add_pod<std::uint32_t>(this->header_crc32, version);
    ::crc32_add_pod<std::uint32_t>(this->header_crc32, header_size);

    ::crc32_add_pod<std::uint64_t>(this->header_crc32, wal_id);
    ::crc32_add_pod<std::uint64_t>(this->header_crc32, start_seq);

    ::crc32_add_pod<std::uint32_t>(this->header_crc32, block_size);
    ::crc32_add_pod<std::uint32_t>(this->header_crc32, reserved);
}

std::uint32_t WALFileHeader::compute_crc32(const WALFileHeader& wal_file_header)
{
    std::uint32_t result = ::crc32(0, Z_NULL, NULL);

    ::crc32_add_pod<std::uint32_t>(result, wal_file_header.magic);
    ::crc32_add_pod<std::uint32_t>(result, wal_file_header.version);
    ::crc32_add_pod<std::uint32_t>(result, wal_file_header.header_size);
                                           
    ::crc32_add_pod<std::uint64_t>(result, wal_file_header.wal_id);
    ::crc32_add_pod<std::uint64_t>(result, wal_file_header.start_seq);
                                           
    ::crc32_add_pod<std::uint32_t>(result, wal_file_header.block_size);
    ::crc32_add_pod<std::uint32_t>(result, wal_file_header.reserved);

    return result;  
}

namespace
{
    constexpr std::size_t kRecordPayloadPrefixSize =
        sizeof(std::uint32_t) + sizeof(std::uint32_t);

    Status check_writer_offset(
        WritableFile& file,
        std::uint64_t expected_offset
    )
    {
        Result<std::uint64_t> position = file.current_position();
        if (!position.is_ok()) {
            return std::move(position.status);
        }

        if (position.value != expected_offset) {
            return Status{
                StatusCode::InvalidOffset,
                "WAL tracked offset " + std::to_string(expected_offset) +
                " differs from writable-file cursor " +
                std::to_string(position.value)
            };
        }

        return Status::ok();
    }


    bool is_valid_fragment_type(Fragment::Type type) noexcept
    {
        switch (type) {
        case Fragment::Type::FULL:
        case Fragment::Type::FIRST:
        case Fragment::Type::MIDDLE:
        case Fragment::Type::LAST:
            return true;
        }

        return false;
    }

    bool is_valid_record_type(::Type type) noexcept
    {
        return type == ::Type::Put || type == ::Type::Tombstone;
    }

    void append_u32_le(
        std::vector<std::byte>& out,
        std::uint32_t value
    )
    {
        out.push_back(static_cast<std::byte>(value & 0xffu));
        out.push_back(static_cast<std::byte>((value >> 8u) & 0xffu));
        out.push_back(static_cast<std::byte>((value >> 16u) & 0xffu));
        out.push_back(static_cast<std::byte>((value >> 24u) & 0xffu));
    }

    Result<std::uint32_t> read_u32_le(
        std::span<const std::byte> bytes,
        std::size_t& position
    )
    {
        if (position > bytes.size() || bytes.size() - position < 4) {
            return Result<std::uint32_t>::fail(
                Status{
                    StatusCode::UnexpectedEOF,
                    "logical WAL record ended inside a u32 length field"
                }
            );
        }

        const auto b0 = std::to_integer<std::uint32_t>(bytes[position + 0]);
        const auto b1 = std::to_integer<std::uint32_t>(bytes[position + 1]);
        const auto b2 = std::to_integer<std::uint32_t>(bytes[position + 2]);
        const auto b3 = std::to_integer<std::uint32_t>(bytes[position + 3]);

        position += 4;

        return Result<std::uint32_t>::ok(
            b0 | (b1 << 8u) | (b2 << 16u) | (b3 << 24u)
        );
    }

    Result<std::vector<std::byte>> encode_record_payload(
        const InternalRecord& record
    )
    {
        if (!is_valid_record_type(record.type)) {
            return Result<std::vector<std::byte>>::fail(
                Status{
                    StatusCode::InvalidArgument,
                    "cannot append a WAL record with an invalid record type"
                }
            );
        }

        if (record.key_entry.size > 0 && record.key_entry.data == nullptr) {
            return Result<std::vector<std::byte>>::fail(
                Status{
                    StatusCode::InvalidArgument,
                    "record key size is non-zero but key data is null"
                }
            );
        }

        if (record.value_entry.size > 0 && record.value_entry.data == nullptr) {
            return Result<std::vector<std::byte>>::fail(
                Status{
                    StatusCode::InvalidArgument,
                    "record value size is non-zero but value data is null"
                }
            );
        }

        const std::size_t key_size = record.key_entry.size;
        const std::size_t value_size = record.value_entry.size;

        if (key_size >
            std::numeric_limits<std::size_t>::max() -
            kRecordPayloadPrefixSize) {
            return Result<std::vector<std::byte>>::fail(
                Status{
                    StatusCode::AllocationTooLarge,
                    "WAL key size overflows payload size"
                }
            );
        }

        const std::size_t prefix_and_key =
            kRecordPayloadPrefixSize + key_size;

        if (value_size >
            std::numeric_limits<std::size_t>::max() - prefix_and_key) {
            return Result<std::vector<std::byte>>::fail(
                Status{
                    StatusCode::AllocationTooLarge,
                    "WAL value size overflows payload size"
                }
            );
        }

        try {
            std::vector<std::byte> payload;
            payload.reserve(prefix_and_key + value_size);

            append_u32_le(payload, record.key_entry.size);
            append_u32_le(payload, record.value_entry.size);

            if (key_size > 0) {
                const auto* key = static_cast<const std::byte*>(
                    record.key_entry.data
                    );
                payload.insert(payload.end(), key, key + key_size);
            }

            if (value_size > 0) {
                const auto* value = static_cast<const std::byte*>(
                    record.value_entry.data
                    );
                payload.insert(payload.end(), value, value + value_size);
            }

            return Result<std::vector<std::byte>>::ok(std::move(payload));
        }
        catch (const std::bad_alloc&) {
            return Result<std::vector<std::byte>>::fail(
                Status{
                    StatusCode::BadAlloc,
                    "failed to allocate encoded WAL record payload"
                }
            );
        }
    }

    Result<InternalRecord> decode_record_payload(
        std::span<const std::byte> payload,
        std::uint64_t seq_num,
        ::Type type,
        Arena& arena
    )
    {
        if (!is_valid_record_type(type)) {
            return Result<InternalRecord>::fail(
                Status{
                    StatusCode::Corruption,
                    "WAL logical record has an invalid record type"
                }
            );
        }

        std::size_t position = 0;

        Result<std::uint32_t> key_size_result =
            read_u32_le(payload, position);
        if (!key_size_result.is_ok()) {
            return Result<InternalRecord>::fail(
                std::move(key_size_result.status)
            );
        }

        Result<std::uint32_t> value_size_result =
            read_u32_le(payload, position);
        if (!value_size_result.is_ok()) {
            return Result<InternalRecord>::fail(
                std::move(value_size_result.status)
            );
        }

        const std::uint32_t key_size = key_size_result.value;
        const std::uint32_t value_size = value_size_result.value;

        const std::uint64_t required =
            static_cast<std::uint64_t>(kRecordPayloadPrefixSize) +
            static_cast<std::uint64_t>(key_size) +
            static_cast<std::uint64_t>(value_size);

        if (required != payload.size()) {
            return Result<InternalRecord>::fail(
                Status{
                    StatusCode::InvalidPayloadSize,
                    "WAL logical payload size does not match its key/value lengths"
                }
            );
        }

        const Arena::Checkpoint checkpoint = arena.checkpoint();

        InternalRecord record{};
        record.seq_num = seq_num;
        record.type = type;
        record.key_entry.size = key_size;
        record.value_entry.size = value_size;

        if (key_size > 0) {
            Result<void*> allocation =
                arena.alloc(key_size, alignof(std::byte));
            if (!allocation.is_ok()) {
                arena.rollback(checkpoint);
                return Result<InternalRecord>::fail(
                    std::move(allocation.status)
                );
            }

            record.key_entry.data = allocation.value;
            std::memcpy(
                record.key_entry.data,
                payload.data() + position,
                key_size
            );
            position += key_size;
        }

        if (value_size > 0) {
            Result<void*> allocation =
                arena.alloc(value_size, alignof(std::byte));
            if (!allocation.is_ok()) {
                arena.rollback(checkpoint);
                return Result<InternalRecord>::fail(
                    std::move(allocation.status)
                );
            }

            record.value_entry.data = allocation.value;
            std::memcpy(
                record.value_entry.data,
                payload.data() + position,
                value_size
            );
            position += value_size;
        }

        return Result<InternalRecord>::ok(std::move(record));
    }

    bool is_recoverable_tail_error(StatusCode code) noexcept
    {
        return code == StatusCode::UnexpectedEOF;
    }

    bool is_corruption_error(StatusCode code) noexcept
    {
        switch (code) {
        case StatusCode::ChecksumMismatch:
        case StatusCode::Corruption:
        case StatusCode::BadMagic:
        case StatusCode::UnsupportedVersion:
        case StatusCode::UnsupportedBlockSize:
        case StatusCode::InvalidHeader:
        case StatusCode::InvalidPayloadSize:
        case StatusCode::InvalidBlockType:
        case StatusCode::InvalidAlignment:
        case StatusCode::OffsetOverlap:
            return true;
        default:
            return false;
        }
    }

    class LogicalRecordAssembler
    {
    public:
        Result<std::optional<InternalRecord>> consume(
            const Fragment& fragment,
            Arena& arena
        )
        {
            const Fragment::Type fragment_type =
                fragment.header.fragment_type;

            if (fragment_type == Fragment::Type::FULL) {
                if (active_) {
                    return fail(
                        "FULL fragment appeared while another logical record was incomplete"
                    );
                }

                Result<InternalRecord> record = decode_record_payload(
                    fragment.payload.bytes,
                    fragment.header.seq_num,
                    fragment.header.type,
                    arena
                );
                if (!record.is_ok()) {
                    return Result<std::optional<InternalRecord>>::fail(
                        std::move(record.status)
                    );
                }

                return Result<std::optional<InternalRecord>>::ok(
                    std::optional<InternalRecord>{
                    std::move(record.value)
                }
                );
            }

            if (fragment_type == Fragment::Type::FIRST) {
                if (active_) {
                    return fail(
                        "FIRST fragment appeared before the previous logical record ended"
                    );
                }

                active_ = true;
                seq_num_ = fragment.header.seq_num;
                type_ = fragment.header.type;
                bytes_.clear();

                Status append_status = append(fragment.payload.bytes);
                if (!append_status.is_ok()) {
                    reset();
                    return Result<std::optional<InternalRecord>>::fail(
                        std::move(append_status)
                    );
                }

                return Result<std::optional<InternalRecord>>::ok(
                    std::nullopt
                );
            }

            if (!active_) {
                return fail(
                    "MIDDLE/LAST fragment appeared without a preceding FIRST fragment"
                );
            }

            if (seq_num_ != fragment.header.seq_num ||
                type_ != fragment.header.type) {
                return fail(
                    "fragment sequence number or record type changed inside one logical record"
                );
            }

            Status append_status = append(fragment.payload.bytes);
            if (!append_status.is_ok()) {
                reset();
                return Result<std::optional<InternalRecord>>::fail(
                    std::move(append_status)
                );
            }

            if (fragment_type == Fragment::Type::MIDDLE) {
                return Result<std::optional<InternalRecord>>::ok(
                    std::nullopt
                );
            }

            if (fragment_type != Fragment::Type::LAST) {
                return fail("invalid fragment type");
            }

            Result<InternalRecord> record = decode_record_payload(
                bytes_,
                seq_num_,
                type_,
                arena
            );
            reset();

            if (!record.is_ok()) {
                return Result<std::optional<InternalRecord>>::fail(
                    std::move(record.status)
                );
            }

            return Result<std::optional<InternalRecord>>::ok(
                std::optional<InternalRecord>{
                std::move(record.value)
            }
            );
        }

        [[nodiscard]] bool active() const noexcept
        {
            return active_;
        }

    private:
        Result<std::optional<InternalRecord>> fail(
            std::string message
        )
        {
            reset();
            return Result<std::optional<InternalRecord>>::fail(
                Status{
                    StatusCode::Corruption,
                    std::move(message)
                }
            );
        }

        Status append(std::span<const std::byte> bytes)
        {
            if (bytes.size() >
                std::numeric_limits<std::size_t>::max() - bytes_.size()) {
                return Status{
                    StatusCode::AllocationTooLarge,
                    "reconstructed WAL record size overflow"
                };
            }

            try {
                bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
                return Status::ok();
            }
            catch (const std::bad_alloc&) {
                return Status{
                    StatusCode::BadAlloc,
                    "failed to grow reconstructed WAL record buffer"
                };
            }
        }

        void reset() noexcept
        {
            active_ = false;
            seq_num_ = 0;
            type_ = ::Type::Put;
            bytes_.clear();
        }

        bool active_ = false;
        std::uint64_t seq_num_ = 0;
        ::Type type_ = ::Type::Put;
        std::vector<std::byte> bytes_;
    };
} // namespace

WALFileHeader::WALFileHeader(
    std::uint32_t new_wal_id,
    std::uint64_t new_start_seq
)
    : magic(WAL_FILE_MAGIC),
    version(WAL_VERSION),
    header_size(WALFileHeader::disk_size()),
    wal_id(new_wal_id),
    start_seq(new_start_seq),
    block_size(WAL_FILE_BLOCK_SIZE),
    reserved(0),
    header_crc32(0)
{
    this->compute_crc32();
}

bool WALFileHeader::self_check() const
{
    return magic == WAL_FILE_MAGIC &&
        version == WAL_VERSION &&
        header_size == WALFileHeader::disk_size() &&
        block_size == WAL_FILE_BLOCK_SIZE &&
        reserved == 0 &&
        header_crc32 == WALFileHeader::compute_crc32(*this);
}

Status WALFileHeader::write(
    WritableFile& file,
    std::uint64_t& offset
) const
{
    if (!self_check()) {
        return Status{
            StatusCode::InvalidHeader,
            "refusing to write an invalid WAL file header"
        };
    }

    Status status = check_writer_offset(file, offset);
    if (!status.is_ok()) {
        return status;
    }

    status = ensure_fits_in_block(
        file,
        WALFileHeader::disk_size(),
        offset,
        WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) {
        return status;
    }

    status = kvdb::blockio::write_u32_t_le(
        file, magic, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u32_t_le(
        file, version, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u32_t_le(
        file, header_size, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u64_t_le(
        file, wal_id, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u64_t_le(
        file, start_seq, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u32_t_le(
        file, block_size, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u32_t_le(
        file, reserved, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u32_t_le(
        file, header_crc32, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    return check_writer_offset(file, offset);
}

Result<std::optional<WALFileHeader>> WALFileHeader::load(
    ReadableFile& file,
    std::uint32_t expected_wal_id,
    std::uint64_t& offset
)
{
    const std::uint64_t initial_offset = offset;

    std::uint64_t file_size = 0;
    Status status = file.get_file_size(file_size);
    if (!status.is_ok()) {
        return Result<std::optional<WALFileHeader>>::fail(
            std::move(status)
        );
    }

    if (offset == file_size) {
        return Result<std::optional<WALFileHeader>>::ok(std::nullopt);
    }

    if (offset > file_size) {
        return Result<std::optional<WALFileHeader>>::fail(
            Status{
                StatusCode::InvalidOffset,
                "WAL header offset is beyond end of file"
            }
        );
    }

    status = ensure_fits_in_block(
        file,
        WALFileHeader::disk_size(),
        offset,
        WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) {
        offset = initial_offset;
        return Result<std::optional<WALFileHeader>>::fail(
            std::move(status)
        );
    }

    if (offset > file_size ||
        file_size - offset < WALFileHeader::disk_size()) {
        offset = initial_offset;
        return Result<std::optional<WALFileHeader>>::fail(
            Status{
                StatusCode::UnexpectedEOF,
                "WAL file ended inside its file header"
            }
        );
    }

    WALFileHeader header{};

    status = kvdb::blockio::read_u32_t_le(
        file, header.magic, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) goto read_failure;

    status = kvdb::blockio::read_u32_t_le(
        file, header.version, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) goto read_failure;

    status = kvdb::blockio::read_u32_t_le(
        file, header.header_size, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) goto read_failure;

    status = kvdb::blockio::read_u64_t_le(
        file, header.wal_id, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) goto read_failure;

    status = kvdb::blockio::read_u64_t_le(
        file, header.start_seq, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) goto read_failure;

    status = kvdb::blockio::read_u32_t_le(
        file, header.block_size, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) goto read_failure;

    status = kvdb::blockio::read_u32_t_le(
        file, header.reserved, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) goto read_failure;

    status = kvdb::blockio::read_u32_t_le(
        file, header.header_crc32, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) goto read_failure;

    if (header.magic != WAL_FILE_MAGIC) {
        offset = initial_offset;
        return Result<std::optional<WALFileHeader>>::fail(
            Status{ StatusCode::BadMagic, "invalid WAL file magic" }
        );
    }

    if (header.version != WAL_VERSION) {
        offset = initial_offset;
        return Result<std::optional<WALFileHeader>>::fail(
            Status{
                StatusCode::UnsupportedVersion,
                "unsupported WAL version " +
                std::to_string(header.version)
            }
        );
    }

    if (header.header_size != WALFileHeader::disk_size()) {
        offset = initial_offset;
        return Result<std::optional<WALFileHeader>>::fail(
            Status{
                StatusCode::InvalidHeader,
                "WAL header_size does not match the supported layout"
            }
        );
    }

    if (header.wal_id != expected_wal_id) {
        offset = initial_offset;
        return Result<std::optional<WALFileHeader>>::fail(
            Status{
                StatusCode::InvalidHeader,
                "WAL id mismatch: expected " +
                std::to_string(expected_wal_id) +
                ", found " + std::to_string(header.wal_id)
            }
        );
    }

    if (header.block_size != WAL_FILE_BLOCK_SIZE) {
        offset = initial_offset;
        return Result<std::optional<WALFileHeader>>::fail(
            Status{
                StatusCode::UnsupportedBlockSize,
                "unsupported WAL block size " +
                std::to_string(header.block_size)
            }
        );
    }

    if (header.reserved != 0) {
        offset = initial_offset;
        return Result<std::optional<WALFileHeader>>::fail(
            Status{
                StatusCode::InvalidHeader,
                "WAL reserved header field must be zero"
            }
        );
    }

    if (header.header_crc32 != WALFileHeader::compute_crc32(header)) {
        offset = initial_offset;
        return Result<std::optional<WALFileHeader>>::fail(
            Status{
                StatusCode::ChecksumMismatch,
                "WAL file-header CRC mismatch"
            }
        );
    }

    return Result<std::optional<WALFileHeader>>::ok(
        std::optional<WALFileHeader>{std::move(header)}
    );

read_failure:
    offset = initial_offset;
    return Result<std::optional<WALFileHeader>>::fail(
        std::move(status)
    );
}

Status Fragment::compute_crc32(
    std::uint32_t& crc32_out,
    const Fragment& fragment
)
{
    if (fragment.header.header_size != Header::disk_size()) {
        return Status{
            StatusCode::InvalidHeader,
            "fragment header_size is invalid"
        };
    }

    if (!is_valid_fragment_type(fragment.header.fragment_type)) {
        return Status{
            StatusCode::InvalidBlockType,
            "fragment type is invalid"
        };
    }

    if (!is_valid_record_type(fragment.header.type)) {
        return Status{
            StatusCode::InvalidArgument,
            "record type in fragment header is invalid"
        };
    }

    if (fragment.header.fragment_size !=
        fragment.payload.bytes.size()) {
        return Status{
            StatusCode::InvalidPayloadSize,
            "fragment_size does not match payload size"
        };
    }

    crc32_out = ::crc32(0L, Z_NULL, 0);
    crc32_add_pod<std::uint32_t>(crc32_out, fragment.header.header_size);
    crc32_add_pod<std::uint8_t>(
        crc32_out,
        static_cast<std::uint8_t>(fragment.header.fragment_type)
    );
    crc32_add_pod<std::uint8_t>(
        crc32_out,
        static_cast<std::uint8_t>(fragment.header.type)
    );
    crc32_add_pod<std::uint64_t>(crc32_out, fragment.header.seq_num);
    crc32_add_pod<std::uint32_t>(crc32_out, fragment.header.fragment_size);
    ::compute_crc32(crc32_out, fragment.payload.bytes.data(), fragment.payload.bytes.size());

    return Status::ok();
}

Status Fragment::compute_crc32()
{
    return Fragment::compute_crc32(
        header.fragment_crc32,
        *this
    );
}

Status Fragment::Header::write(
    WritableFile& file,
    std::uint64_t& offset
) const
{
    if (header_size != Header::disk_size()) {
        return Status{
            StatusCode::InvalidHeader,
            "cannot write fragment with invalid header_size"
        };
    }

    if (!is_valid_fragment_type(fragment_type)) {
        return Status{
            StatusCode::InvalidBlockType,
            "cannot write invalid WAL fragment type"
        };
    }

    if (!is_valid_record_type(type)) {
        return Status{
            StatusCode::InvalidArgument,
            "cannot write invalid WAL record type"
        };
    }

    Status status = check_writer_offset(file, offset);
    if (!status.is_ok()) {
        return status;
    }

    status = ensure_fits_in_block(
        file,
        Header::disk_size(),
        offset,
        WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) {
        return status;
    }

    status = kvdb::blockio::write_u32_t_le(
        file, fragment_crc32, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u32_t_le(
        file, header_size, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u8_t(
        file,
        static_cast<std::uint8_t>(fragment_type),
        offset,
        WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u8_t(
        file,
        static_cast<std::uint8_t>(type),
        offset,
        WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u64_t_le(
        file, seq_num, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    return kvdb::blockio::write_u32_t_le(
        file, fragment_size, offset, WAL_FILE_BLOCK_SIZE
    );
}

Result<Fragment::Header> Fragment::Header::load(
    ReadableFile& file,
    std::uint64_t& offset
)
{
    const std::uint64_t initial_offset = offset;

    Status status = ensure_fits_in_block(
        file,
        Header::disk_size(),
        offset,
        WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) {
        offset = initial_offset;
        return Result<Header>::fail(std::move(status));
    }

    Header header{};

    status = kvdb::blockio::read_u32_t_le(
        file, header.fragment_crc32, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) goto read_failure;

    status = kvdb::blockio::read_u32_t_le(
        file, header.header_size, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) goto read_failure;

    {
        std::uint8_t fragment_type = 0;
        status = kvdb::blockio::read_u8_t(
            file, fragment_type, offset, WAL_FILE_BLOCK_SIZE
        );
        if (!status.is_ok()) goto read_failure;
        header.fragment_type =
            static_cast<Fragment::Type>(fragment_type);
    }

    {
        std::uint8_t record_type = 0;
        status = kvdb::blockio::read_u8_t(
            file, record_type, offset, WAL_FILE_BLOCK_SIZE
        );
        if (!status.is_ok()) goto read_failure;
        header.type = static_cast<::Type>(record_type);
    }

    status = kvdb::blockio::read_u64_t_le(
        file, header.seq_num, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) goto read_failure;

    status = kvdb::blockio::read_u32_t_le(
        file, header.fragment_size, offset, WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) goto read_failure;

    if (header.header_size != Header::disk_size()) {
        offset = initial_offset;
        return Result<Header>::fail(
            Status{
                StatusCode::InvalidHeader,
                "WAL fragment header_size is invalid"
            }
        );
    }

    if (!is_valid_fragment_type(header.fragment_type)) {
        offset = initial_offset;
        return Result<Header>::fail(
            Status{
                StatusCode::InvalidBlockType,
                "WAL fragment type is invalid"
            }
        );
    }

    if (!is_valid_record_type(header.type)) {
        offset = initial_offset;
        return Result<Header>::fail(
            Status{
                StatusCode::Corruption,
                "WAL fragment contains an invalid record type"
            }
        );
    }

    return Result<Header>::ok(std::move(header));

read_failure:
    offset = initial_offset;
    return Result<Header>::fail(std::move(status));
}

Status Fragment::Payload::write(
    WritableFile& file,
    std::uint64_t& offset
)
{
    Status status = check_writer_offset(file, offset);
    if (!status.is_ok()) {
        return status;
    }

    status = ensure_fits_in_block(
        file,
        bytes.size(),
        offset,
        WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) {
        return status;
    }

    if (bytes.empty()) {
        return Status::ok();
    }

    return kvdb::blockio::write_bytes(
        file,
        std::span<std::byte>(bytes.data(), bytes.size()),
        offset,
        WAL_FILE_BLOCK_SIZE
    );
}

Result<Fragment::Payload> Fragment::Payload::load(
    ReadableFile& file,
    std::uint32_t size,
    std::uint64_t& offset
)
{
    Status status = fits_in_block(
        offset,
        size,
        WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) {
        return Result<Payload>::fail(
            Status{
                StatusCode::InvalidPayloadSize,
                "WAL fragment payload crosses a block boundary"
            }
        );
    }

    Payload payload{};

    try {
        payload.bytes.resize(size);
    }
    catch (const std::bad_alloc&) {
        return Result<Payload>::fail(
            Status{
                StatusCode::BadAlloc,
                "failed to allocate WAL fragment payload"
            }
        );
    }

    if (size == 0) {
        return Result<Payload>::ok(std::move(payload));
    }

    status = kvdb::blockio::read_bytes(
        file,
        payload.bytes.data(),
        size,
        offset,
        WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) {
        return Result<Payload>::fail(std::move(status));
    }

    return Result<Payload>::ok(std::move(payload));
}

Status Fragment::write(
    WritableFile& file,
    std::uint64_t& offset
)
{
    if (header.fragment_size != payload.bytes.size()) {
        return Status{
            StatusCode::InvalidPayloadSize,
            "fragment header size and payload size differ"
        };
    }

    if (disk_size() > WAL_FILE_BLOCK_SIZE) {
        return Status{
            StatusCode::SizeExceedsBlockSize,
            "WAL fragment exceeds one block"
        };
    }

    Status status = check_writer_offset(file, offset);
    if (!status.is_ok()) {
        return status;
    }

    // Ensure the header and payload move together. Calling ensure separately
    // for each part could otherwise align the payload into the next block.
    status = ensure_fits_in_block(
        file,
        disk_size(),
        offset,
        WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) {
        return status;
    }

    status = header.write(file, offset);
    if (!status.is_ok()) {
        return status;
    }

    status = payload.write(file, offset);
    if (!status.is_ok()) {
        return status;
    }

    return check_writer_offset(file, offset);
}

Result<std::optional<Fragment>> Fragment::load(
    ReadableFile& file,
    std::uint64_t& offset
)
{
    const std::uint64_t initial_offset = offset;

    std::uint64_t file_size = 0;
    Status status = file.get_file_size(file_size);
    if (!status.is_ok()) {
        return Result<std::optional<Fragment>>::fail(
            std::move(status)
        );
    }

    if (offset == file_size) {
        return Result<std::optional<Fragment>>::ok(std::nullopt);
    }

    if (offset > file_size) {
        return Result<std::optional<Fragment>>::fail(
            Status{
                StatusCode::InvalidOffset,
                "WAL fragment offset is beyond end of file"
            }
        );
    }

    status = ensure_fits_in_block(
        file,
        Header::disk_size() + 1u,
        offset,
        WAL_FILE_BLOCK_SIZE
    );
    if (!status.is_ok()) {
        offset = initial_offset;
        return Result<std::optional<Fragment>>::fail(
            std::move(status)
        );
    }

    // The old offset may have pointed into block padding.
    if (offset >= file_size) {
        offset = file_size;
        return Result<std::optional<Fragment>>::ok(std::nullopt);
    }

    if (file_size - offset < Header::disk_size()) {
        offset = initial_offset;
        return Result<std::optional<Fragment>>::fail(
            Status{
                StatusCode::UnexpectedEOF,
                "WAL file ended inside a fragment header"
            }
        );
    }

    const std::uint64_t fragment_start = offset;

    Result<Header> header_result = Header::load(file, offset);
    if (!header_result.is_ok()) {
        offset = initial_offset;
        return Result<std::optional<Fragment>>::fail(
            std::move(header_result.status)
        );
    }

    Header header = std::move(header_result.value);

    const std::uint64_t in_block = offset % WAL_FILE_BLOCK_SIZE;
    const std::uint64_t available_in_block =
        WAL_FILE_BLOCK_SIZE - in_block;

    if (header.fragment_size > available_in_block) {
        offset = initial_offset;
        return Result<std::optional<Fragment>>::fail(
            Status{
                StatusCode::InvalidPayloadSize,
                "WAL fragment payload crosses a block boundary"
            }
        );
    }

    if (header.fragment_size >
        WAL_FILE_BLOCK_SIZE - Header::disk_size()) {
        offset = initial_offset;
        return Result<std::optional<Fragment>>::fail(
            Status{
                StatusCode::InvalidPayloadSize,
                "WAL fragment payload is larger than a block can hold"
            }
        );
    }

    if (file_size - offset < header.fragment_size) {
        offset = initial_offset;
        return Result<std::optional<Fragment>>::fail(
            Status{
                StatusCode::UnexpectedEOF,
                "WAL file ended inside a fragment payload"
            }
        );
    }

    Result<Payload> payload_result = Payload::load(
        file,
        header.fragment_size,
        offset
    );
    if (!payload_result.is_ok()) {
        offset = initial_offset;
        return Result<std::optional<Fragment>>::fail(
            std::move(payload_result.status)
        );
    }

    Fragment fragment{};
    fragment.header = std::move(header);
    fragment.payload = std::move(payload_result.value);

    std::uint32_t expected_crc = 0;
    status = Fragment::compute_crc32(expected_crc, fragment);
    if (!status.is_ok()) {
        offset = initial_offset;
        return Result<std::optional<Fragment>>::fail(
            std::move(status)
        );
    }

    if (expected_crc != fragment.header.fragment_crc32) {
        offset = initial_offset;
        return Result<std::optional<Fragment>>::fail(
            Status{
                StatusCode::ChecksumMismatch,
                "WAL fragment CRC mismatch at offset " +
                std::to_string(fragment_start)
            }
        );
    }

    return Result<std::optional<Fragment>>::ok(
        std::optional<Fragment>{std::move(fragment)}
    );
}

WALWriter::~WALWriter()
{
    if (file_ != nullptr) {
        // A destructor cannot report an error. Normal code should call close().
        (void)file_->close();
    }
}

Status WALWriter::create(
    const std::filesystem::path& path,
    std::uint32_t new_wal_id,
    std::uint64_t start_seq
)
{
    if (path.empty()) {
        return Status{
            StatusCode::InvalidArgument,
            "WAL path is empty"
        };
    }

    if (file_ != nullptr) {
        Status close_status = close();
        if (!close_status.is_ok()) {
            return close_status;
        }
    }

    Result<std::unique_ptr<WritableFile>> open_result;
    try {
        open_result = open_writable_file(path);
    }
    catch (const std::bad_alloc&) {
        return Status{
            StatusCode::BadAlloc,
            "failed to allocate writable WAL file"
        };
    }
    catch (const std::exception& exception) {
        return Status{
            StatusCode::OpenFailed,
            "failed to open WAL for writing: " +
            std::string(exception.what())
        };
    }

    if (!open_result.is_ok()) {
        return std::move(open_result.status);
    }

    if (!open_result.value) {
        return Status{
            StatusCode::BadFileDescriptor,
            "opening WAL returned a null writable file"
        }; 
    }

    file_ = std::move(open_result.value);
    path_ = path;
    wal_id_ = new_wal_id;
    offset_ = 0;

    WALFileHeader header(new_wal_id, start_seq);
    Status status = header.write(*file_, offset_);
    if (!status.is_ok()) {
        (void)file_->close();
        file_.reset();
        path_.clear();
        wal_id_ = 0;
        offset_ = 0;
        return status;
    }

    status = file_->sync();
    if (!status.is_ok()) {
        (void)file_->close();
        file_.reset();
        path_.clear();
        wal_id_ = 0;
        offset_ = 0;
        return status;
    }

    return Status::ok();
}

Status WALWriter::rotate(
    const std::filesystem::path& new_path,
    std::uint32_t new_wal_id,
    std::uint64_t start_seq
)
{
    Status close_status = close();
    if (!close_status.is_ok()) {
        return close_status;
    }

    return create(new_path, new_wal_id, start_seq);
}

Status WALWriter::write(const InternalRecord& record)
{
    if (file_ == nullptr) {
        return Status{
            StatusCode::FailedPrecondition,
            "WAL writer is not open"
        };
    }

    Status status = check_writer_offset(*file_, offset_);
    if (!status.is_ok()) {
        return status;
    }

    Result<std::vector<std::byte>> payload =
        encode_record_payload(record);
    if (!payload.is_ok()) {
        return std::move(payload.status);
    }

    return write_payload(payload.value, record);
}

Status WALWriter::write_payload(
    std::span<const std::byte> byte_sequence,
    const InternalRecord& record
)
{
    if (file_ == nullptr) {
        return Status{
            StatusCode::FailedPrecondition,
            "WAL writer is not open"
        };
    }

    if (byte_sequence.empty()) {
        return Status{
            StatusCode::InvalidArgument,
            "encoded WAL logical record cannot be empty"
        };
    }

    std::size_t payload_position = 0;
    bool first = true;

    while (payload_position < byte_sequence.size()) {
        Status status = check_writer_offset(*file_, offset_);
        if (!status.is_ok()) {
            return status;
        }

        status = ensure_fits_in_block(
            *file_,
            Fragment::Header::disk_size() + 1,
            offset_,
            WAL_FILE_BLOCK_SIZE
        );
        if (!status.is_ok()) {
            return status;
        }

        const std::uint64_t remaining_in_block =
            WAL_FILE_BLOCK_SIZE -
            (offset_ % WAL_FILE_BLOCK_SIZE);

        if (remaining_in_block < Fragment::Header::disk_size()) {
            return Status{
                StatusCode::InvariantViolation,
                "WAL header alignment left too little room for a header"
            };
        }

        const std::uint64_t max_payload_in_fragment =
            remaining_in_block - Fragment::Header::disk_size();

        if (max_payload_in_fragment == 0) {
            return Status{
                StatusCode::InvariantViolation,
                "WAL fragment has no payload capacity"
            };
        }

        const std::size_t payload_left =
            byte_sequence.size() - payload_position;

        const std::size_t fragment_payload_size =
            std::min<std::size_t>(
                payload_left,
                static_cast<std::size_t>(max_payload_in_fragment)
            );

        const bool last =
            fragment_payload_size == payload_left;

        Fragment fragment{};

        if (first && last) {
            fragment.header.fragment_type =
                Fragment::Type::FULL;
        }
        else if (first) {
            fragment.header.fragment_type =
                Fragment::Type::FIRST;
        }
        else if (last) {
            fragment.header.fragment_type =
                Fragment::Type::LAST;
        }
        else {
            fragment.header.fragment_type =
                Fragment::Type::MIDDLE;
        }

        fragment.header.header_size =
            Fragment::Header::disk_size();
        fragment.header.type = record.type;
        fragment.header.seq_num = record.seq_num;
        fragment.header.fragment_size =
            static_cast<std::uint32_t>(fragment_payload_size);

        try {
            fragment.payload.bytes.assign(
                byte_sequence.begin() +
                static_cast<std::ptrdiff_t>(payload_position),
                byte_sequence.begin() +
                static_cast<std::ptrdiff_t>(
                    payload_position + fragment_payload_size
                    )
            );
        }
        catch (const std::bad_alloc&) {
            return Status{
                StatusCode::BadAlloc,
                "failed to allocate WAL fragment"
            };
        }

        status = fragment.compute_crc32();
        if (!status.is_ok()) {
            return status;
        }

        status = fragment.write(*file_, offset_);
        if (!status.is_ok()) {
            return status;
        }

        status = check_writer_offset(*file_, offset_);
        if (!status.is_ok()) {
            return status;
        }

        payload_position += fragment_payload_size;
        first = false;
    }

    return Status::ok();
}

Status WALWriter::sync()
{
    if (file_ == nullptr) {
        return Status{
            StatusCode::FailedPrecondition,
            "WAL writer is not open"
        };
    }

    Status status = check_writer_offset(*file_, offset_);
    if (!status.is_ok()) {
        return status;
    }

    return file_->sync();
}

Status WALWriter::close()
{
    if (file_ == nullptr) {
        return Status::ok();
    }

    Status sync_status = file_->sync();
    Status close_status = file_->close();

    file_.reset();
    path_.clear();
    wal_id_ = 0;
    offset_ = 0;

    if (!sync_status.is_ok()) {
        return sync_status;
    }

    return close_status;
}

Result<WALLoader::LoadResult> WALLoader::load(
    ReadableFile& file,
    std::uint64_t& offset,
    std::uint32_t expected_wal_id,
    Arena& arena
)
{
    LoadResult result{};

    Result<std::optional<WALFileHeader>> header_result =
        WALFileHeader::load(file, expected_wal_id, offset);
    if (!header_result.is_ok()) {
        return Result<LoadResult>::fail(
            std::move(header_result.status)
        );
    }

    if (!header_result.value.has_value()) {
        return Result<LoadResult>::fail(
            Status{
                StatusCode::UnexpectedEOF,
                "WAL file is empty and has no header"
            }
        );
    }

    result.header = std::move(header_result.value);

    LogicalRecordAssembler assembler;

    while (true) {

        Result<std::optional<Fragment>> fragment_result =
            Fragment::load(file, offset);

        if (!fragment_result.is_ok()) {
            const StatusCode code = fragment_result.status.code;
            result.error = fragment_result.status.message;

            if (is_recoverable_tail_error(code)) {
                result.had_torn_tail = true;
                result.ok = true;
                break;
            }

            if (is_corruption_error(code)) {
                result.had_corruption = true;
                result.ok = false;
                break;
            }

            return Result<LoadResult>::fail(
                std::move(fragment_result.status)
            );
        }

        if (!fragment_result.value.has_value()) {
            if (assembler.active()) {
                result.had_torn_tail = true;
                result.error =
                    "WAL ended before the LAST fragment of a logical record";
            }
            break;
        }

        Result<std::optional<InternalRecord>> assembled =
            assembler.consume(*fragment_result.value, arena);

        if (!assembled.is_ok()) {
            result.had_corruption = true;
            result.ok = false;
            result.error = assembled.status.message;
            break;
        }

        if (!assembled.value.has_value()) {
            continue;
        }

        try {
            result.records.emplace_back(
                std::move(*assembled.value)
            );
        }
        catch (const std::bad_alloc&) {
            return Result<LoadResult>::fail(
                Status{
                    StatusCode::BadAlloc,
                    "failed to store recovered WAL record"
                }
            );
        }

    }

    return Result<LoadResult>::ok(std::move(result));
}

Result<WALLoader::LoadResult> WALLoader::load(
    const std::filesystem::path& path,
    std::uint32_t expected_wal_id,
    Arena& arena
)
{
    if (path.empty()) {
        return Result<LoadResult>::fail(
            Status{
                StatusCode::InvalidArgument,
                "WAL path is empty"
            }
        );
    }

    Result<std::unique_ptr<ReadableFile>> open_result;
    try {
        open_result = open_readable_file(path);
    }
    catch (const std::bad_alloc&) {
        return Result<LoadResult>::fail(
            Status{
                StatusCode::BadAlloc,
                "failed to allocate readable WAL file"
            }
        );
    }
    catch (const std::exception& exception) {
        return Result<LoadResult>::fail(
            Status{
                StatusCode::OpenFailed,
                "failed to open WAL for recovery: " +
                std::string(exception.what())
            }
        );
    }

    if (!open_result.is_ok()) {
        return Result<LoadResult>::fail(
            std::move(open_result.status)
        );
    }

    if (!open_result.value)
        return Result<LoadResult>::fail(
            Status{
                StatusCode::BadFileDescriptor,
                "opening WAL returned a null readable file"
            }
        );

    std::unique_ptr<ReadableFile> file =
        std::move(open_result.value);

    std::uint64_t offset = 0;
    Result<LoadResult> load_result =
        WALLoader::load(*file, offset, expected_wal_id, arena);

    Status close_status = file->close();

    if (!load_result.is_ok()) {
        return load_result;
    }

    if (!close_status.is_ok()) {
        return Result<LoadResult>::fail(
            std::move(close_status)
        );
    }

    return load_result;
}

//WALStreamingLoader::WALStreamingLoader(const std::filesystem::path& path, Arena& arena)
//    : path(path), arena(arena)
//{
//}

Status WALStreamingLoader::open()
{
    if (file_) {
        return Status{
            StatusCode::InvariantViolation,
            "WAL streaming loader is already open"
        };
    }

    Result<std::unique_ptr<ReadableFile>> open_result =
        ::open_readable_file(path_);

    if (!open_result.is_ok()) {
        return open_result.status;
    }

    if (!open_result.value) {
        return Status{
            StatusCode::BadFileDescriptor,
            "opening WAL returned a null readable file"
        };
    }

    file_ = std::move(open_result.value);

    result_ = LoadResult{};
    terminal_error_.clear();
    state_ = State::Ready;

    return Status::ok();
}

Status WALStreamingLoader::validate_state(
    std::uint64_t offset,
    std::uint32_t expected_wal_id
) const
{
    if (!file_) {
        return Status{
            StatusCode::InvariantViolation,
            "WAL streaming loader is not open"
        };
    }

    if (state_ != State::Ready) {
        return Status{
            StatusCode::InvariantViolation,
            "attempted to validate a terminal WAL loader state"
        };
    }

    std::uint64_t file_size = 0;

    Status status = file_->get_file_size(file_size);
    if (!status.is_ok()) {
        return status;
    }

    if (offset > file_size) {
        return Status{
            StatusCode::InvalidOffset,
            std::format(
                "manual offset {} is beyond WAL file size {}",
                offset,
                file_size
            )
        };
    }

    if (!result_.header.has_value()) {
        if (offset != 0) {
            return Status{
                StatusCode::InvariantViolation,
                std::format(
                    "WAL header has not been loaded, but offset is {}",
                    offset
                )
            };
        }

        return Status::ok();
    }

    if (offset == 0) {
        return Status{
            StatusCode::InvariantViolation,
            "WAL header is loaded, but offset is zero"
        };
    }

    if (result_.header->wal_id != expected_wal_id) {
        return Status{
            StatusCode::InvariantViolation,
            std::format(
                "expected WAL id {}, but loaded WAL id is {}",
                expected_wal_id,
                result_.header->wal_id
            )
        };
    }

    return Status::ok();
}
void WALStreamingLoader::reset_call_result()
{
    result_.logical_record.reset();

    result_.reached_eof = false;
    result_.ok = true;
    result_.had_torn_tail = false;
    result_.had_corruption = false;

    result_.error.clear();
}

void WALStreamingLoader::mark_torn_tail(std::string message)
{
    state_ = State::TornTail;
    terminal_error_ = std::move(message);

    result_.ok = true;
    result_.had_torn_tail = true;
    result_.had_corruption = false;
    result_.reached_eof = false;
    result_.error = terminal_error_;
}

void WALStreamingLoader::mark_corruption(std::string message)
{
    state_ = State::Corrupted;
    terminal_error_ = std::move(message);

    result_.ok = false;
    result_.had_torn_tail = false;
    result_.had_corruption = true;
    result_.reached_eof = false;
    result_.error = terminal_error_;
}
Status WALStreamingLoader::load_next(
    std::uint64_t& offset,
    std::uint32_t expected_wal_id
)
{
    reset_call_result();

    /*
     * Terminal states are idempotent.
     *
     * Calling load_next() again after EOF, torn tail, or corruption
     * reports the same terminal condition without reading again.
     */
    switch (state_)
    {
        case State::Closed:
            return Status{
                StatusCode::InvariantViolation,
                "WAL streaming loader is not open"
            };

        case State::EndOfFile:
            result_.reached_eof = true;
            return Status::ok();

        case State::TornTail:
            result_.had_torn_tail = true;
            result_.error = terminal_error_;
            return Status::ok();

        case State::Corrupted:
            result_.ok = false;
            result_.had_corruption = true;
            result_.error = terminal_error_;
            return Status::ok();

        case State::Ready:
            break;
    }

    Status status = validate_state(offset, expected_wal_id);
    if (!status.is_ok()) {
        return status;
    }

    /*
     * Load the file header once.
     */
    if (!result_.header.has_value())
    {
        Result<std::optional<WALFileHeader>> header_result =
            WALFileHeader::load(
                *file_,
                expected_wal_id,
                offset
            );

        if (!header_result.is_ok())
        {
            const StatusCode code = header_result.status.code;

            if (is_recoverable_tail_error(code)) {
                mark_torn_tail(header_result.status.message);
                return Status::ok();
            }

            if (is_corruption_error(code)) {
                mark_corruption(header_result.status.message);
                return Status::ok();
            }

            return header_result.status;
        }

        if (!header_result.value.has_value())
        {
            mark_torn_tail(
                "WAL ended before a complete file header was read"
            );

            return Status::ok();
        }

        result_.header = std::move(*header_result.value);

        /*
         * WALFileHeader::load() should already validate this, but keeping
         * this check protects the loader's invariant.
         */
        if (result_.header->wal_id != expected_wal_id)
        {
            mark_corruption(
                std::format(
                    "expected WAL id {}, but file header contains WAL id {}",
                    expected_wal_id,
                    result_.header->wal_id
                )
            );

            return Status::ok();
        }

        /*
         * A complete header is now part of the valid WAL prefix.
         */
    }

    /*
     * Every load_next() call creates a new assembler because it reads
     * exactly one complete logical record.
     */
    LogicalRecordAssembler assembler;

    /*
     * Replace these names with your Arena checkpoint API if necessary.
     *
     * Any memory allocated for an incomplete or corrupted logical record
     * must be reclaimed.
     */
    const auto arena_checkpoint = arena_.checkpoint();

    while (true)
    {
        Result<std::optional<Fragment>> fragment_result =
            Fragment::load(*file_, offset);

        if (!fragment_result.is_ok())
        {
            arena_.rollback(arena_checkpoint);

            const StatusCode code = fragment_result.status.code;

            if (is_recoverable_tail_error(code))
            {
                mark_torn_tail(
                    fragment_result.status.message
                );

                return Status::ok();
            }

            if (is_corruption_error(code))
            {
                mark_corruption(
                    fragment_result.status.message
                );

                return Status::ok();
            }

            /*
             * Even for an ordinary I/O failure, discard allocations made
             * for the partially assembled record.
             */
            return fragment_result.status;
        }

        /*
         * Fragment::load() returning nullopt means physical EOF.
         */
        if (!fragment_result.value.has_value())
        {
            arena_.rollback(arena_checkpoint);

            if (assembler.active())
            {
                mark_torn_tail(
                    "WAL ended before the LAST fragment of a logical record"
                );

                return Status::ok();
            }

            state_ = State::EndOfFile;
            result_.reached_eof = true;

            return Status::ok();
        }

        Result<std::optional<InternalRecord>> assembled_result =
            assembler.consume(
                *fragment_result.value,
                arena_
            );

        if (!assembled_result.is_ok())
        {
            arena_.rollback(arena_checkpoint);

            mark_corruption(
                assembled_result.status.message
            );

            return Status::ok();
        }

        /*
         * FIRST or MIDDLE fragment: continue consuming.
         */
        if (!assembled_result.value.has_value()) {
            continue;
        }

        /*
         * FULL fragment or completed FIRST/MIDDLE/LAST sequence.
         */
        result_.logical_record =
            std::move(*assembled_result.value);

        /*
         * The record is complete, so do not roll the arena back.
         * The InternalRecord may reference arena-owned memory.
         */

        return Status::ok();
    }
}