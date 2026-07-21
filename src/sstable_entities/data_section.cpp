#include "sstable_entities/data_section.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <new>
#include <span>
#include <utility>

#include "endian_io.h"
#include "file_helpers.h"

using namespace SSTableEntities;

namespace
{
    [[nodiscard]] constexpr std::uint64_t payload_disk_size_u64(
        const DataSection::Payload& payload
    ) noexcept
    {
        return static_cast<std::uint64_t>(
            DataSection::Payload::fixed_part_disk_size()
            ) + static_cast<std::uint64_t>(payload.key_size) +
            static_cast<std::uint64_t>(payload.value_size);
    }

    [[nodiscard]] bool is_valid_record_type(::Type type) noexcept
    {
        return type == ::Type::Put || type == ::Type::Tombstone;
    }

    [[nodiscard]] int compare_keys(
        const DataSection::Payload& lhs,
        const DataSection::Payload& rhs
    ) noexcept
    {
        const std::size_t common = std::min<std::size_t>(
            lhs.key_size,
            rhs.key_size
        );

        if (common > 0) {
            const int compared = std::memcmp(lhs.key_ptr, rhs.key_ptr, common);
            if (compared < 0) {
                return -1;
            }
            if (compared > 0) {
                return 1;
            }
        }

        if (lhs.key_size < rhs.key_size) {
            return -1;
        }
        if (lhs.key_size > rhs.key_size) {
            return 1;
        }
        return 0;
    }

    [[nodiscard]] Status validate_order(
        const DataSection::Payload& previous,
        const DataSection::Payload& current
    )
    {
        const int key_order = compare_keys(previous, current);

        if (key_order > 0) {
            return Status{
                StatusCode::InvalidArgument,
                "data records must be appended in ascending key order"
            };
        }

        if (key_order == 0) {
            if (current.seq_num > previous.seq_num) {
                return Status{
                    StatusCode::InvalidArgument,
                    "equal keys must be appended in descending sequence order"
                };
            }

            if (current.seq_num == previous.seq_num) {
                return Status{
                    StatusCode::Duplicate,
                    "duplicate internal record key and sequence number"
                };
            }
        }

        return Status::ok();
    }
}

DataSection::Header::Header() noexcept
    : type(BlockType::Data),
    payload_disk_size(0),
    crc32(::crc32(0L, Z_NULL, 0))
{
}

DataSection::Payload::Payload(const InternalRecord& record) noexcept
    : key_size(static_cast<std::uint32_t>(record.key_entry.size)),
    value_size(static_cast<std::uint32_t>(record.value_entry.size)),
    type(record.type),
    flags(0),
    reserved(0),
    seq_num(record.seq_num),
    key_ptr(record.key_entry.data),
    value_ptr(record.value_entry.data)
{
}

std::size_t DataSection::Payload::disk_size() const noexcept
{
    return static_cast<std::size_t>(payload_disk_size_u64(*this));
}

Status DataSection::Payload::validate() const
{
    if (key_size > 0 && key_ptr == nullptr) {
        return Status{
            StatusCode::NullPointer,
            "data payload key pointer is null while key_size is non-zero"
        };
    }

    if (value_size > 0 && value_ptr == nullptr) {
        return Status{
            StatusCode::NullPointer,
            "data payload value pointer is null while value_size is non-zero"
        };
    }

    if (!is_valid_record_type(type)) {
        return Status{
            StatusCode::InvalidArgument,
            "data payload record type is invalid"
        };
    }

    if (flags != 0 || reserved != 0) {
        return Status{
            StatusCode::InvalidArgument,
            "data payload flags and reserved fields must be zero for SSTable version 1"
        };
    }

    const std::uint64_t payload_size = payload_disk_size_u64(*this);
    constexpr std::uint64_t capacity =
        static_cast<std::uint64_t>(BLOCK_SIZE) - Header::disk_size();

    if (payload_size > capacity) {
        return Status{
            StatusCode::InvalidPayloadSize,
            std::format(
                "data payload exceeds block capacity: payload_size={}, capacity={}",
                payload_size,
                capacity
            )
        };
    }

    return Status::ok();
}

Status DataSection::Header::write(
    WritableFile& file,
    std::uint64_t& offset
) const
{
    Result<std::uint64_t> position_result = file.current_position();
    if (!position_result.is_ok()) {
        return std::move(position_result.status);
    }

    if (position_result.value != offset) {
        return Status{
            StatusCode::InvalidOffset,
            "tracked data-header offset does not match the writable file cursor"
        };
    }

    if (offset % BLOCK_SIZE != 0) {
        return Status{
            StatusCode::InvalidBlockAlignment,
            "data block header must start at a physical block boundary"
        };
    }

    if (type != BlockType::Data) {
        return Status{
            StatusCode::InvalidBlockType,
            "data block header has a non-data block type"
        };
    }

    if (payload_disk_size > BLOCK_SIZE - disk_size()) {
        return Status{
            StatusCode::InvalidPayloadSize,
            "data block header payload size exceeds physical block capacity"
        };
    }

    Status status = fits_in_block(offset, disk_size(), BLOCK_SIZE);
    if (!status.is_ok()) {
        return status;
    }

    status = kvdb::blockio::write_u8_t(
        file,
        static_cast<std::uint8_t>(type),
        offset,
        BLOCK_SIZE
    );
    if (!status.is_ok()) {
        return status;
    }

    status = kvdb::blockio::write_u32_t_le(
        file,
        payload_disk_size,
        offset,
        BLOCK_SIZE
    );
    if (!status.is_ok()) {
        return status;
    }

    return kvdb::blockio::write_u32_t_le(
        file,
        crc32,
        offset,
        BLOCK_SIZE
    );
}

Status DataSection::Payload::write(
    WritableFile& file,
    std::uint64_t& offset
) const
{
    Status status = validate();
    if (!status.is_ok()) {
        return status;
    }

    Result<std::uint64_t> position_result = file.current_position();
    if (!position_result.is_ok()) {
        return std::move(position_result.status);
    }

    if (position_result.value != offset) {
        return Status{
            StatusCode::InvalidOffset,
            "tracked data-payload offset does not match the writable file cursor"
        };
    }

    // One logical payload is indivisible. Do not let block I/O silently align
    // halfway through it when assertions are disabled.
    status = fits_in_block(offset, disk_size(), BLOCK_SIZE);
    if (!status.is_ok()) {
        return status;
    }

    status = kvdb::blockio::write_u32_t_le(file, key_size, offset, BLOCK_SIZE);
    if (!status.is_ok()) {
        return status;
    }

    status = kvdb::blockio::write_u32_t_le(file, value_size, offset, BLOCK_SIZE);
    if (!status.is_ok()) {
        return status;
    }

    status = kvdb::blockio::write_u8_t(
        file,
        static_cast<std::uint8_t>(type),
        offset,
        BLOCK_SIZE
    );
    if (!status.is_ok()) {
        return status;
    }

    status = kvdb::blockio::write_u32_t_le(file, flags, offset, BLOCK_SIZE);
    if (!status.is_ok()) {
        return status;
    }

    status = kvdb::blockio::write_u32_t_le(file, reserved, offset, BLOCK_SIZE);
    if (!status.is_ok()) {
        return status;
    }

    status = kvdb::blockio::write_u64_t_le(file, seq_num, offset, BLOCK_SIZE);
    if (!status.is_ok()) {
        return status;
    }

    if (key_size > 0) {
        status = kvdb::blockio::write_bytes(
            file,
            std::span<const std::byte>(
                static_cast<const std::byte*>(key_ptr),
                key_size
            ),
            offset,
            BLOCK_SIZE
        );
        if (!status.is_ok()) {
            return status;
        }
    }

    if (value_size > 0) {
        status = kvdb::blockio::write_bytes(
            file,
            std::span<const std::byte>(
                static_cast<const std::byte*>(value_ptr),
                value_size
            ),
            offset,
            BLOCK_SIZE
        );
        if (!status.is_ok()) {
            return status;
        }
    }

    return Status::ok();
}

std::size_t DataSection::DataBlock::disk_size() const noexcept
{
    std::size_t result = Header::disk_size();
    for (const Payload& payload : payloads) {
        result += payload.disk_size();
    }
    return result;
}

bool DataSection::DataBlock::can_payload_fit(
    const Payload& payload
) const noexcept
{
    const std::uint64_t resulting_size =
        static_cast<std::uint64_t>(disk_size()) +
        payload_disk_size_u64(payload);

    return resulting_size <= BLOCK_SIZE;
}

Status DataSection::DataBlock::validate() const
{
    if (payloads.empty()) {
        return Status{
            StatusCode::InvalidState,
            "data block cannot be written without payloads"
        };
    }

    std::uint64_t payload_bytes = 0;
    const Payload* previous = nullptr;

    for (const Payload& payload : payloads) {
        Status status = payload.validate();
        if (!status.is_ok()) {
            return status;
        }

        if (previous != nullptr) {
            status = validate_order(*previous, payload);
            if (!status.is_ok()) {
                return status;
            }
        }

        payload_bytes += payload_disk_size_u64(payload);
        if (payload_bytes >
            static_cast<std::uint64_t>(BLOCK_SIZE) - Header::disk_size()) {
            return Status{
                StatusCode::InvalidPayloadSize,
                std::format(
                    "data block exceeds physical block size: block_size={}, capacity={}",
                    payload_bytes + Header::disk_size(),
                    BLOCK_SIZE
                )
            };
        }

        previous = &payload;
    }

    return Status::ok();
}

Status DataSection::DataBlock::add_payload(const Payload& payload)
{
    Status status = payload.validate();
    if (!status.is_ok()) {
        return status;
    }

    if (!payloads.empty()) {
        status = validate_order(payloads.back(), payload);
        if (!status.is_ok()) {
            return status;
        }
    }

    if (!can_payload_fit(payload)) {
        return Status{
            StatusCode::InvalidPayloadSize,
            std::format(
                "data payload does not fit in current block: block_size={}, payload_size={}, capacity={}",
                disk_size(),
                payload.disk_size(),
                BLOCK_SIZE
            )
        };
    }

    try {
        payloads.emplace_back(payload);
    }
    catch (const std::bad_alloc&) {
        return Status{
            StatusCode::BadAlloc,
            "failed to append payload to data block"
        };
    }

    status = rebuild_header();
    if (!status.is_ok()) {
        payloads.pop_back();
        if (!payloads.empty()) {
            (void)rebuild_header();
        }
        else {
            header = Header{};
        }
        return status;
    }

    return Status::ok();
}

Status DataSection::DataBlock::rebuild_header()
{
    Status status = validate();
    if (!status.is_ok()) {
        return status;
    }

    std::uint64_t payload_bytes = 0;
    for (const Payload& payload : payloads) {
        payload_bytes += payload_disk_size_u64(payload);
    }

    if (payload_bytes > std::numeric_limits<std::uint32_t>::max()) {
        return Status{
            StatusCode::DataTypeOverflow,
            "data block payload byte count cannot be represented by uint32_t"
        };
    }

    header.type = BlockType::Data;
    header.payload_disk_size = static_cast<std::uint32_t>(payload_bytes);
    calculate_crc32(header.crc32);

    return Status::ok();
}

Status DataSection::DataBlock::write(
    WritableFile& file,
    std::uint64_t& offset,
    IndexSection& index_section
)
{
    Status status = rebuild_header();
    if (!status.is_ok()) {
        return status;
    }

    Result<std::uint64_t> position_result = file.current_position();
    if (!position_result.is_ok()) {
        return std::move(position_result.status);
    }

    if (position_result.value != offset) {
        return Status{
            StatusCode::InvalidOffset,
            "tracked data-block offset does not match the writable file cursor"
        };
    }

    status = align_to_block_boundary(file, offset, BLOCK_SIZE);
    if (!status.is_ok()) {
        return status;
    }

    const std::uint64_t data_block_offset = offset;

    status = header.write(file, offset);
    if (!status.is_ok()) {
        return status;
    }

    for (const Payload& payload : payloads) {
        status = payload.write(file, offset);
        if (!status.is_ok()) {
            return status;
        }
    }

    const Payload& first = payloads.front();
    const Payload& last = payloads.back();

    try {
        index_section.add_index(
            data_block_offset,
            first.key_size,
            last.key_size,
            first.key_ptr,
            last.key_ptr
        );
    }
    catch (const std::bad_alloc&) {
        return Status{
            StatusCode::BadAlloc,
            "failed to append the data block's derived index entry"
        };
    }

    return Status::ok();
}

void DataSection::Payload::append_crc32(
    std::uint32_t& crc_buffer
) const noexcept
{
    crc32_add_pod<std::uint32_t>(crc_buffer, key_size);
    crc32_add_pod<std::uint32_t>(crc_buffer, value_size);
    crc32_add_pod<std::uint8_t>(
        crc_buffer,
        static_cast<std::uint8_t>(type)
    );
    crc32_add_pod<std::uint32_t>(crc_buffer, flags);
    crc32_add_pod<std::uint32_t>(crc_buffer, reserved);
    crc32_add_pod<std::uint64_t>(crc_buffer, seq_num);

    if (key_size > 0) {
        compute_crc32(crc_buffer, key_ptr, key_size);
    }
    if (value_size > 0) {
        compute_crc32(crc_buffer, value_ptr, value_size);
    }
}

void DataSection::Payload::calculate_crc32(
    std::uint32_t& crc_buffer
) const noexcept
{
    crc_buffer = ::crc32(0L, Z_NULL, 0);
    append_crc32(crc_buffer);
}

void DataSection::DataBlock::calculate_crc32(
    std::uint32_t& crc_buffer
) const noexcept
{
    crc_buffer = ::crc32(0L, Z_NULL, 0);
    for (const Payload& payload : payloads) {
        payload.append_crc32(crc_buffer);
    }
}

void DataSection::init_new_block()
{
    data_blocks.emplace_back();
}

Status DataSection::add_payload(const InternalRecord& record)
{
    if (record.key_entry.size >
        std::numeric_limits<std::uint32_t>::max() ||
        record.value_entry.size >
        std::numeric_limits<std::uint32_t>::max()) {
        return Status{
            StatusCode::DataTypeOverflow,
            "record key/value size cannot be represented by the SSTable uint32 fields"
        };
    }

    const Payload payload(record);

    Status status = payload.validate();
    if (!status.is_ok()) {
        return status;
    }

    const Payload* previous = nullptr;
    for (auto block_it = data_blocks.rbegin();
        block_it != data_blocks.rend();
        ++block_it) {
        if (!block_it->payloads.empty()) {
            previous = &block_it->payloads.back();
            break;
        }
    }

    if (previous != nullptr) {
        status = validate_order(*previous, payload);
        if (!status.is_ok()) {
            return status;
        }
    }

    if (!data_blocks.empty() && data_blocks.back().can_payload_fit(payload)) {
        return data_blocks.back().add_payload(payload);
    }

    DataBlock new_block;
    status = new_block.add_payload(payload);
    if (!status.is_ok()) {
        return status;
    }

    try {
        data_blocks.emplace_back(std::move(new_block));
    }
    catch (const std::bad_alloc&) {
        return Status{
            StatusCode::BadAlloc,
            "failed to append a new physical data block"
        };
    }

    return Status::ok();
}

Status DataSection::validate() const
{
    const Payload* previous_block_last = nullptr;

    for (const DataBlock& block : data_blocks) {
        Status status = block.validate();
        if (!status.is_ok()) {
            return status;
        }

        if (previous_block_last != nullptr) {
            status = validate_order(
                *previous_block_last,
                block.payloads.front()
            );
            if (!status.is_ok()) {
                return status;
            }
        }

        previous_block_last = &block.payloads.back();
    }

    return Status::ok();
}

std::size_t DataSection::disk_size() const noexcept
{
    std::size_t result = 0;
    for (const DataBlock& block : data_blocks) {
        result += block.disk_size();
    }
    return result;
}

std::size_t DataSection::physical_span() const noexcept
{
    if (data_blocks.empty()) {
        return 0;
    }

    return (data_blocks.size() - 1) *
        static_cast<std::size_t>(BLOCK_SIZE) +
        data_blocks.back().disk_size();
}

Status DataSection::write(
    WritableFile& file,
    std::uint64_t& offset,
    IndexSection& index_section,
    std::uint64_t& data_offset
)
{
    Status status = validate();
    if (!status.is_ok()) {
        return status;
    }

    Result<std::uint64_t> position_result = file.current_position();
    if (!position_result.is_ok()) {
        return std::move(position_result.status);
    }

    if (position_result.value != offset) {
        return Status{
            StatusCode::InvalidOffset,
            "tracked data-section offset does not match the writable file cursor"
        };
    }

    // Empty sections are a no-op. Do not add block padding for zero data blocks.
    if (data_blocks.empty()) {
        IndexSection empty_index;
        index_section = std::move(empty_index);
        data_offset = offset;
        return Status::ok();
    }

    // Rebuild all derived state before the first byte is written.
    for (DataBlock& block : data_blocks) {
        status = block.rebuild_header();
        if (!status.is_ok()) {
            return status;
        }
    }

    IndexSection staged_index;
    try {
        staged_index.payloads.reserve(data_blocks.size());
    }
    catch (const std::bad_alloc&) {
        return Status{
            StatusCode::BadAlloc,
            "failed to reserve derived data-block index entries"
        };
    }

    status = align_to_block_boundary(file, offset, BLOCK_SIZE);
    if (!status.is_ok()) {
        return status;
    }

    const std::uint64_t staged_data_offset = offset;

    for (DataBlock& block : data_blocks) {
        status = block.write(file, offset, staged_index);
        if (!status.is_ok()) {
            return status;
        }
    }

    index_section = std::move(staged_index);
    data_offset = staged_data_offset;

    return Status::ok();
}