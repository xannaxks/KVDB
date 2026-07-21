#include "sstable_entities/data_section_view.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <utility>

#include "crc32_helpers.h"
#include "endian_io.h"
#include "file_helpers.h"

using namespace SSTableEntities;

namespace
{
    constexpr std::size_t kHeaderSize = DataSection::Header::disk_size();
    constexpr std::size_t kRecordFixedSize =
        DataSection::Payload::fixed_part_disk_size();

    [[nodiscard]] bool checked_add_u64(
        std::uint64_t lhs,
        std::uint64_t rhs,
        std::uint64_t& result
    ) noexcept
    {
        if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs) {
            return false;
        }
        result = lhs + rhs;
        return true;
    }

    [[nodiscard]] bool checked_mul_u64(
        std::uint64_t lhs,
        std::uint64_t rhs,
        std::uint64_t& result
    ) noexcept
    {
        if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
            return false;
        }
        result = lhs * rhs;
        return true;
    }

    [[nodiscard]] std::uint8_t byte_value(std::byte value) noexcept
    {
        return std::to_integer<std::uint8_t>(value);
    }

    [[nodiscard]] bool read_u8(
        std::span<const std::byte> bytes,
        std::size_t& position,
        std::uint8_t& value
    ) noexcept
    {
        if (position >= bytes.size()) {
            return false;
        }
        value = byte_value(bytes[position]);
        ++position;
        return true;
    }

    [[nodiscard]] bool read_u32_le(
        std::span<const std::byte> bytes,
        std::size_t& position,
        std::uint32_t& value
    ) noexcept
    {
        if (bytes.size() - position < sizeof(std::uint32_t)) {
            return false;
        }

        value = static_cast<std::uint32_t>(byte_value(bytes[position])) |
            (static_cast<std::uint32_t>(byte_value(bytes[position + 1])) << 8U) |
            (static_cast<std::uint32_t>(byte_value(bytes[position + 2])) << 16U) |
            (static_cast<std::uint32_t>(byte_value(bytes[position + 3])) << 24U);
        position += sizeof(std::uint32_t);
        return true;
    }

    [[nodiscard]] bool read_u64_le(
        std::span<const std::byte> bytes,
        std::size_t& position,
        std::uint64_t& value
    ) noexcept
    {
        if (bytes.size() - position < sizeof(std::uint64_t)) {
            return false;
        }

        value = 0;
        for (std::size_t i = 0; i < sizeof(std::uint64_t); ++i) {
            value |= static_cast<std::uint64_t>(byte_value(bytes[position + i]))
                << (i * 8U);
        }
        position += sizeof(std::uint64_t);
        return true;
    }

    [[nodiscard]] bool is_valid_record_type(::Type type) noexcept
    {
        return type == ::Type::Put || type == ::Type::Tombstone;
    }

    [[nodiscard]] int compare_keys(
        std::span<const std::byte> lhs,
        std::span<const std::byte> rhs
    ) noexcept
    {
        const std::size_t common = std::min(lhs.size(), rhs.size());
        if (common > 0) {
            const int result = std::memcmp(lhs.data(), rhs.data(), common);
            if (result < 0) {
                return -1;
            }
            if (result > 0) {
                return 1;
            }
        }

        if (lhs.size() < rhs.size()) {
            return -1;
        }
        if (lhs.size() > rhs.size()) {
            return 1;
        }
        return 0;
    }

    [[nodiscard]] Status corruption(std::string message)
    {
        return Status{ StatusCode::Corruption, std::move(message) };
    }
}

std::uint64_t DataSectionView::Header::used_size() const noexcept
{
    return static_cast<std::uint64_t>(kHeaderSize) + header.payload_disk_size;
}

Result<DataSectionView::Header> DataSectionView::Header::load(
    ReadableFile& file,
    std::uint64_t& offset,
    std::uint64_t section_end_offset
)
{
    const std::uint64_t original_offset = offset;

    if (offset % BLOCK_SIZE != 0) {
        return Result<Header>::fail(Status{
            StatusCode::InvalidAlignment,
            "data block header offset is not block aligned"
            });
    }

    std::uint64_t header_end = 0;
    if (!checked_add_u64(offset, kHeaderSize, header_end) ||
        header_end > section_end_offset) {
        return Result<Header>::fail(Status{
            StatusCode::UnexpectedEOF,
            "data block header lies outside the declared data section"
            });
    }

    Header result{};
    result.header_offset = offset;

    std::uint8_t encoded_type = 0;
    Status status = kvdb::blockio::read_u8_t(
        file,
        encoded_type,
        offset,
        BLOCK_SIZE
    );
    if (!status.is_ok()) {
        offset = original_offset;
        return Result<Header>::fail(std::move(status));
    }

    result.header.type = static_cast<BlockType>(encoded_type);
    status = kvdb::blockio::read_u32_t_le(
        file,
        result.header.payload_disk_size,
        offset,
        BLOCK_SIZE
    );
    if (!status.is_ok()) {
        offset = original_offset;
        return Result<Header>::fail(std::move(status));
    }

    status = kvdb::blockio::read_u32_t_le(
        file,
        result.header.crc32,
        offset,
        BLOCK_SIZE
    );
    if (!status.is_ok()) {
        offset = original_offset;
        return Result<Header>::fail(std::move(status));
    }

    if (result.header.type != BlockType::Data) {
        offset = original_offset;
        return Result<Header>::fail(Status{
            StatusCode::InvalidBlockType,
            "expected a data block while discovering DataSectionView"
            });
    }

    constexpr std::uint32_t payload_capacity =
        static_cast<std::uint32_t>(BLOCK_SIZE - kHeaderSize);
    if (result.header.payload_disk_size > payload_capacity) {
        offset = original_offset;
        return Result<Header>::fail(Status{
            StatusCode::InvalidPayloadSize,
            "data block payload size exceeds physical block capacity"
            });
    }

    result.payload_offset = offset;
    if (!checked_add_u64(
        result.payload_offset,
        result.header.payload_disk_size,
        result.payload_end_offset)) {
        offset = original_offset;
        return Result<Header>::fail(Status{
            StatusCode::OffsetOutOfRange,
            "data block payload end overflows uint64_t"
            });
    }

    if (!checked_add_u64(
        result.header_offset,
        BLOCK_SIZE,
        result.next_block_offset)) {
        offset = original_offset;
        return Result<Header>::fail(Status{
            StatusCode::OffsetOutOfRange,
            "next data block offset overflows uint64_t"
            });
    }

    if (result.payload_end_offset > result.next_block_offset ||
        result.next_block_offset > section_end_offset) {
        offset = original_offset;
        return Result<Header>::fail(Status{
            StatusCode::OffsetOverlap,
            "data block extends outside its physical block or declared section"
            });
    }

    return Result<Header>::ok(std::move(result));
}

std::uint64_t DataSectionView::RecordMeta::disk_size() const noexcept
{
    return static_cast<std::uint64_t>(kRecordFixedSize) + key_size + value_size;
}

std::uint64_t DataSectionView::DataBlock::used_size() const noexcept
{
    return header_view.used_size();
}

DataSectionView::DataBlock::ValidationState
DataSectionView::DataBlock::validation_state() const noexcept
{
    return validation_state_;
}

std::size_t DataSectionView::DataBlock::record_count() const noexcept
{
    return validation_state_ == ValidationState::Valid ? records_.size() : 0;
}

Result<DataSectionView::DataBlock> DataSectionView::DataBlock::load(
    ReadableFile& file,
    std::uint64_t& offset,
    std::uint64_t section_end_offset
)
{
    const std::uint64_t original_offset = offset;

    Result<Header> header_result = Header::load(
        file,
        offset,
        section_end_offset
    );
    if (!header_result.is_ok()) {
        offset = original_offset;
        return Result<DataBlock>::fail(std::move(header_result.status));
    }

    DataBlock result{};
    result.header_view = std::move(header_result.value);

    // Payload remains unread. Advance only the caller's logical discovery cursor.
    offset = result.header_view.next_block_offset;
    return Result<DataBlock>::ok(std::move(result));
}

Status DataSectionView::DataBlock::validate(ReadableFile& file) const
{
    if (validation_state_ == ValidationState::Valid) {
        return Status::ok();
    }
    if (validation_state_ == ValidationState::Corrupt &&
        cached_corruption_.has_value()) {
        return *cached_corruption_;
    }

    const std::uint32_t payload_size = header_view.header.payload_disk_size;
    constexpr std::uint32_t payload_capacity =
        static_cast<std::uint32_t>(BLOCK_SIZE - kHeaderSize);

    if (payload_size > payload_capacity) {
        Status status = corruption(
            "data block payload exceeds physical block capacity"
        );
        validation_state_ = ValidationState::Corrupt;
        cached_corruption_ = status;
        return status;
    }

    std::array<std::byte, BLOCK_SIZE> storage{};
    std::uint64_t read_offset = header_view.payload_offset;
    Status status = kvdb::blockio::read_bytes(
        file,
        storage.data(),
        payload_size,
        read_offset,
        BLOCK_SIZE
    );
    if (!status.is_ok()) {
        // Do not permanently cache transient file-system failures.
        return status;
    }

    if (read_offset != header_view.payload_end_offset) {
        return Status{
            StatusCode::InvalidOffset,
            "data payload read did not end at the declared payload boundary"
        };
    }

    std::uint32_t actual_crc = ::crc32(0L, Z_NULL, 0);
    if (payload_size > 0) {
        compute_crc32(actual_crc, storage.data(), payload_size);
    }

    if (actual_crc != header_view.header.crc32) {
        status = Status{
            StatusCode::ChecksumMismatch,
            "data block payload CRC does not match its header"
        };
        validation_state_ = ValidationState::Corrupt;
        cached_corruption_ = status;
        return status;
    }

    if (payload_size < kRecordFixedSize) {
        status = corruption(
            "data block payload is too small to contain one complete record"
        );
        validation_state_ = ValidationState::Corrupt;
        cached_corruption_ = status;
        return status;
    }

    const std::span<const std::byte> bytes(storage.data(), payload_size);
    std::vector<RecordMeta> staged_records;

    try {
        staged_records.reserve(payload_size / kRecordFixedSize);
    }
    catch (const std::bad_alloc&) {
        return Status{
            StatusCode::BadAlloc,
            "failed to reserve lazy data-record metadata"
        };
    }

    std::size_t position = 0;
    std::span<const std::byte> previous_key{};
    std::uint64_t previous_seq = 0;
    bool has_previous = false;

    while (position < bytes.size()) {
        const std::size_t record_start = position;
        RecordMeta meta{};
        std::uint8_t encoded_type = 0;
        std::uint32_t flags = 0;
        std::uint32_t reserved = 0;

        if (bytes.size() - position < kRecordFixedSize ||
            !read_u32_le(bytes, position, meta.key_size) ||
            !read_u32_le(bytes, position, meta.value_size) ||
            !read_u8(bytes, position, encoded_type) ||
            !read_u32_le(bytes, position, flags) ||
            !read_u32_le(bytes, position, reserved) ||
            !read_u64_le(bytes, position, meta.seq_num)) {
            status = corruption("truncated fixed fields in data record");
            validation_state_ = ValidationState::Corrupt;
            cached_corruption_ = status;
            return status;
        }

        meta.type = static_cast<::Type>(encoded_type);
        if (!is_valid_record_type(meta.type)) {
            status = corruption("data record has an invalid record type");
            validation_state_ = ValidationState::Corrupt;
            cached_corruption_ = status;
            return status;
        }
        if (flags != 0 || reserved != 0) {
            status = corruption(
                "data record flags and reserved fields must be zero in version 1"
            );
            validation_state_ = ValidationState::Corrupt;
            cached_corruption_ = status;
            return status;
        }

        const std::uint64_t variable_size =
            static_cast<std::uint64_t>(meta.key_size) + meta.value_size;
        if (variable_size > bytes.size() - position) {
            status = corruption(
                "data record key/value lengths exceed the remaining block payload"
            );
            validation_state_ = ValidationState::Corrupt;
            cached_corruption_ = status;
            return status;
        }

        const std::size_t key_position = position;
        const std::size_t value_position = position + meta.key_size;
        const std::span<const std::byte> current_key(
            bytes.data() + key_position,
            meta.key_size
        );

        if (has_previous) {
            const int key_order = compare_keys(previous_key, current_key);
            if (key_order > 0) {
                status = corruption(
                    "data records are not ordered by ascending user key"
                );
                validation_state_ = ValidationState::Corrupt;
                cached_corruption_ = status;
                return status;
            }
            if (key_order == 0 && meta.seq_num >= previous_seq) {
                status = corruption(
                    "equal data keys are not ordered by strictly descending sequence"
                );
                validation_state_ = ValidationState::Corrupt;
                cached_corruption_ = status;
                return status;
            }
        }

        meta.record_offset = header_view.payload_offset + record_start;
        meta.key_offset = header_view.payload_offset + key_position;
        meta.value_offset = header_view.payload_offset + value_position;

        position += static_cast<std::size_t>(variable_size);

        try {
            staged_records.emplace_back(meta);
        }
        catch (const std::bad_alloc&) {
            return Status{
                StatusCode::BadAlloc,
                "failed to cache lazy data-record metadata"
            };
        }

        previous_key = current_key;
        previous_seq = meta.seq_num;
        has_previous = true;
    }

    if (staged_records.empty() || position != bytes.size()) {
        status = corruption(
            "data block payload does not contain a canonical sequence of records"
        );
        validation_state_ = ValidationState::Corrupt;
        cached_corruption_ = status;
        return status;
    }

    records_ = std::move(staged_records);
    cached_corruption_.reset();
    validation_state_ = ValidationState::Valid;
    return Status::ok();
}

Result<InternalRecord> DataSectionView::DataBlock::read_record(
    ReadableFile& file,
    std::size_t record_index,
    Arena& arena
) const
{
    Status status = validate(file);
    if (!status.is_ok()) {
        return Result<InternalRecord>::fail(std::move(status));
    }

    if (record_index >= records_.size()) {
        return Result<InternalRecord>::fail(Status{
            StatusCode::InvalidArgument,
            "data record index is outside the validated block"
            });
    }

    const RecordMeta& meta = records_[record_index];
    const Arena::Checkpoint checkpoint = arena.checkpoint();

    InternalRecord record{};
    record.type = meta.type;
    record.seq_num = meta.seq_num;
    record.key_entry.size = meta.key_size;
    record.value_entry.size = meta.value_size;

    if (meta.key_size > 0) {
        Result<void*> allocation = arena.alloc(meta.key_size, alignof(std::byte));
        if (!allocation.is_ok()) {
            arena.rollback(checkpoint);
            return Result<InternalRecord>::fail(std::move(allocation.status));
        }

        record.key_entry.data = allocation.value;
        std::uint64_t key_offset = meta.key_offset;
        status = kvdb::blockio::read_bytes(
            file,
            static_cast<std::byte*>(record.key_entry.data),
            meta.key_size,
            key_offset,
            BLOCK_SIZE
        );
        if (!status.is_ok() || key_offset != meta.value_offset) {
            arena.rollback(checkpoint);
            if (!status.is_ok()) {
                return Result<InternalRecord>::fail(std::move(status));
            }
            return Result<InternalRecord>::fail(Status{
                StatusCode::InvalidOffset,
                "key read did not end at the validated value offset"
                });
        }
    }

    if (meta.value_size > 0) {
        Result<void*> allocation = arena.alloc(meta.value_size, alignof(std::byte));
        if (!allocation.is_ok()) {
            arena.rollback(checkpoint);
            return Result<InternalRecord>::fail(std::move(allocation.status));
        }

        record.value_entry.data = allocation.value;
        std::uint64_t value_offset = meta.value_offset;
        status = kvdb::blockio::read_bytes(
            file,
            static_cast<std::byte*>(record.value_entry.data),
            meta.value_size,
            value_offset,
            BLOCK_SIZE
        );
        if (!status.is_ok() ||
            value_offset != meta.record_offset + meta.disk_size()) {
            arena.rollback(checkpoint);
            if (!status.is_ok()) {
                return Result<InternalRecord>::fail(std::move(status));
            }
            return Result<InternalRecord>::fail(Status{
                StatusCode::InvalidOffset,
                "value read did not end at the validated record boundary"
                });
        }
    }

    return Result<InternalRecord>::ok(std::move(record));
}

Result<std::optional<std::size_t>>
DataSectionView::DataBlock::find_first_record(
    ReadableFile& file,
    const ArenaEntry& key
) const
{
    if (key.size > 0 && key.data == nullptr) {
        return Result<std::optional<std::size_t>>::fail(Status{
            StatusCode::InvalidArgument,
            "searched key has a non-zero size but a null data pointer"
            });
    }

    Status status = validate(file);
    if (!status.is_ok()) {
        return Result<std::optional<std::size_t>>::fail(std::move(status));
    }

    const std::uint32_t payload_size = header_view.header.payload_disk_size;
    constexpr std::uint32_t payload_capacity =
        static_cast<std::uint32_t>(BLOCK_SIZE - kHeaderSize);

    if (payload_size > payload_capacity) {
        return Result<std::optional<std::size_t>>::fail(corruption(
            "validated data block payload exceeds physical block capacity"
        ));
    }

    std::array<std::byte, BLOCK_SIZE> storage{};
    std::uint64_t read_offset = header_view.payload_offset;
    status = kvdb::blockio::read_bytes(
        file,
        storage.data(),
        payload_size,
        read_offset,
        BLOCK_SIZE
    );
    if (!status.is_ok()) {
        return Result<std::optional<std::size_t>>::fail(std::move(status));
    }

    if (read_offset != header_view.payload_end_offset) {
        return Result<std::optional<std::size_t>>::fail(Status{
            StatusCode::InvalidOffset,
            "data payload search read ended at an unexpected offset"
            });
    }

    const std::span<const std::byte> searched_key(
        static_cast<const std::byte*>(key.data),
        key.size
    );

    auto record_key = [&](const RecordMeta& meta)
        -> Result<std::span<const std::byte>>
        {
            if (meta.key_offset < header_view.payload_offset) {
                return Result<std::span<const std::byte>>::fail(corruption(
                    "record key offset precedes its block payload"
                ));
            }

            const std::uint64_t relative_u64 =
                meta.key_offset - header_view.payload_offset;

            if (relative_u64 > payload_size ||
                meta.key_size > payload_size - relative_u64) {
                return Result<std::span<const std::byte>>::fail(corruption(
                    "record key range lies outside its validated block payload"
                ));
            }

            const std::size_t relative =
                static_cast<std::size_t>(relative_u64);

            return Result<std::span<const std::byte>>::ok(
                std::span<const std::byte>(
                    storage.data() + relative,
                    meta.key_size
                )
            );
        };

    std::size_t first = 0;
    std::size_t last = records_.size();

    while (first < last) {
        const std::size_t middle = first + (last - first) / 2;
        Result<std::span<const std::byte>> current =
            record_key(records_[middle]);

        if (!current.is_ok()) {
            return Result<std::optional<std::size_t>>::fail(
                std::move(current.status)
            );
        }

        if (compare_keys(current.value, searched_key) < 0) {
            first = middle + 1;
        }
        else {
            last = middle;
        }
    }

    if (first == records_.size()) {
        return Result<std::optional<std::size_t>>::ok(std::nullopt);
    }

    Result<std::span<const std::byte>> candidate =
        record_key(records_[first]);
    if (!candidate.is_ok()) {
        return Result<std::optional<std::size_t>>::fail(
            std::move(candidate.status)
        );
    }

    if (compare_keys(candidate.value, searched_key) != 0) {
        return Result<std::optional<std::size_t>>::ok(std::nullopt);
    }

    // Equal user keys are validated in strictly descending sequence order,
    // so lower_bound returns the newest record for this key.
    return Result<std::optional<std::size_t>>::ok(first);
}

Result<DataSectionView> DataSectionView::load(
    ReadableFile& file,
    std::uint64_t& offset,
    std::uint64_t first_block_offset,
    std::uint64_t data_block_count,
    std::uint64_t expected_section_end_offset
)
{
    const std::uint64_t original_offset = offset;

    std::uint64_t file_size = 0;
    Status status = file.get_file_size(file_size);
    if (!status.is_ok()) {
        return Result<DataSectionView>::fail(std::move(status));
    }

    if (first_block_offset > file_size) {
        return Result<DataSectionView>::fail(Status{
            StatusCode::OffsetOutOfRange,
            "first data block offset lies beyond end of file"
            });
    }

    if (data_block_count > 0 && first_block_offset % BLOCK_SIZE != 0) {
        return Result<DataSectionView>::fail(Status{
            StatusCode::InvalidAlignment,
            "non-empty data section must start at a block boundary"
            });
    }

    std::uint64_t reserved_bytes = 0;
    std::uint64_t computed_section_end = 0;
    if (!checked_mul_u64(data_block_count, BLOCK_SIZE, reserved_bytes) ||
        !checked_add_u64(
            first_block_offset,
            reserved_bytes,
            computed_section_end)) {
        return Result<DataSectionView>::fail(Status{
            StatusCode::InvalidSectionSize,
            "data block count overflows the data section extent"
            });
    }

    if (computed_section_end > file_size) {
        return Result<DataSectionView>::fail(Status{
            StatusCode::InvalidSectionSize,
            "declared data blocks are not fully present in the file"
            });
    }

    if (expected_section_end_offset != 0) {
        if (expected_section_end_offset > file_size ||
            expected_section_end_offset % BLOCK_SIZE != 0) {
            return Result<DataSectionView>::fail(Status{
                StatusCode::InvalidSectionSize,
                "trusted data section end is invalid or outside the file"
                });
        }

        if (data_block_count > 0 &&
            expected_section_end_offset != computed_section_end) {
            return Result<DataSectionView>::fail(Status{
                StatusCode::InvalidSectionSize,
                "data block count does not end at the trusted next-section offset"
                });
        }

        if (data_block_count == 0 &&
            first_block_offset > expected_section_end_offset) {
            return Result<DataSectionView>::fail(Status{
                StatusCode::InvalidSectionSize,
                "empty data section offset lies after the next section"
                });
        }
    }

    if (data_block_count >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return Result<DataSectionView>::fail(Status{
            StatusCode::InvalidSectionSize,
            "data block count cannot be represented by size_t"
            });
    }

    DataSectionView result{};
    result.first_data_block_offset = first_block_offset;
    result.section_end_offset = data_block_count == 0 &&
        expected_section_end_offset != 0
        ? expected_section_end_offset
        : computed_section_end;

    try {
        result.data_blocks.reserve(static_cast<std::size_t>(data_block_count));
    }
    catch (const std::bad_alloc&) {
        return Result<DataSectionView>::fail(Status{
            StatusCode::BadAlloc,
            "failed to reserve lazy data-block metadata"
            });
    }

    std::uint64_t cursor = first_block_offset;
    for (std::uint64_t i = 0; i < data_block_count; ++i) {
        Result<DataBlock> block = DataBlock::load(
            file,
            cursor,
            computed_section_end
        );
        if (!block.is_ok()) {
            offset = original_offset;
            return Result<DataSectionView>::fail(std::move(block.status));
        }

        try {
            result.data_blocks.emplace_back(std::move(block.value));
        }
        catch (const std::bad_alloc&) {
            offset = original_offset;
            return Result<DataSectionView>::fail(Status{
                StatusCode::BadAlloc,
                "failed to append lazy data-block metadata"
                });
        }
    }

    if (cursor != computed_section_end) {
        offset = original_offset;
        return Result<DataSectionView>::fail(Status{
            StatusCode::InvalidSectionSize,
            "lazy data-block discovery ended at an unexpected offset"
            });
    }

    offset = data_block_count == 0 ? first_block_offset : computed_section_end;
    return Result<DataSectionView>::ok(std::move(result));
}

Result<std::uint64_t> DataSectionView::logical_size() const
{
    std::uint64_t total = 0;
    for (const DataBlock& block : data_blocks) {
        if (!checked_add_u64(total, block.used_size(), total)) {
            return Result<std::uint64_t>::fail(Status{
                StatusCode::InvalidSectionSize,
                "logical data section size overflows uint64_t"
                });
        }
    }
    return Result<std::uint64_t>::ok(total);
}

Result<std::uint64_t> DataSectionView::physical_span() const
{
    if (data_blocks.empty()) {
        return Result<std::uint64_t>::ok(0);
    }

    std::uint64_t preceding_blocks = 0;
    if (!checked_mul_u64(
        static_cast<std::uint64_t>(data_blocks.size() - 1),
        BLOCK_SIZE,
        preceding_blocks)) {
        return Result<std::uint64_t>::fail(Status{
            StatusCode::InvalidSectionSize,
            "physical data section span overflows uint64_t"
            });
    }

    std::uint64_t result = 0;
    if (!checked_add_u64(
        preceding_blocks,
        data_blocks.back().used_size(),
        result)) {
        return Result<std::uint64_t>::fail(Status{
            StatusCode::InvalidSectionSize,
            "physical data section span overflows uint64_t"
            });
    }

    return Result<std::uint64_t>::ok(result);
}

Result<std::uint64_t> DataSectionView::reserved_span() const
{
    std::uint64_t result = 0;
    if (!checked_mul_u64(
        static_cast<std::uint64_t>(data_blocks.size()),
        BLOCK_SIZE,
        result)) {
        return Result<std::uint64_t>::fail(Status{
            StatusCode::InvalidSectionSize,
            "reserved data section span overflows uint64_t"
            });
    }
    return Result<std::uint64_t>::ok(result);
}

Status DataSectionView::validate_block(
    ReadableFile& file,
    std::size_t block_index
) const
{
    if (block_index >= data_blocks.size()) {
        return Status{
            StatusCode::InvalidArgument,
            "data block index lies outside DataSectionView"
        };
    }
    return data_blocks[block_index].validate(file);
}

Result<InternalRecord> DataSectionView::read_record(
    ReadableFile& file,
    std::size_t block_index,
    std::size_t record_index,
    Arena& arena
) const
{
    if (block_index >= data_blocks.size()) {
        return Result<InternalRecord>::fail(Status{
            StatusCode::InvalidArgument,
            "data block index lies outside DataSectionView"
            });
    }

    return data_blocks[block_index].read_record(
        file,
        record_index,
        arena
    );
}

Result<std::optional<std::size_t>> DataSectionView::find_first_record(
    ReadableFile& file,
    std::size_t block_index,
    const ArenaEntry& key
) const
{
    if (block_index >= data_blocks.size()) {
        return Result<std::optional<std::size_t>>::fail(Status{
            StatusCode::InvalidArgument,
            "data block index lies outside DataSectionView"
            });
    }

    return data_blocks[block_index].find_first_record(file, key);
}