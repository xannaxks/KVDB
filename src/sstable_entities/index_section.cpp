#include "sstable_entities/index_section.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace SSTableEntities;

namespace
{
    constexpr std::uint64_t kHeaderSize = IndexSection::Header::disk_size();
    constexpr std::uint64_t kPayloadCapacity =
        static_cast<std::uint64_t>(BLOCK_SIZE) - kHeaderSize;

    struct ParsedPayload
    {
        std::uint64_t data_block_offset = 0;
        std::uint32_t first_key_size = 0;
        std::uint32_t last_key_size = 0;
        std::size_t first_key_position = 0;
        std::size_t last_key_position = 0;
    };

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
        if (lhs != 0 &&
            rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
            return false;
        }

        result = lhs * rhs;
        return true;
    }

    [[nodiscard]] std::uint8_t byte_value(std::byte value) noexcept
    {
        return std::to_integer<std::uint8_t>(value);
    }

    [[nodiscard]] bool read_u32_le(
        std::span<const std::byte> bytes,
        std::size_t& position,
        std::uint32_t& value
    ) noexcept
    {
        if (position > bytes.size() ||
            bytes.size() - position < sizeof(std::uint32_t)) {
            return false;
        }

        value =
            static_cast<std::uint32_t>(byte_value(bytes[position])) |
            (static_cast<std::uint32_t>(
                byte_value(bytes[position + 1])) << 8U) |
            (static_cast<std::uint32_t>(
                byte_value(bytes[position + 2])) << 16U) |
            (static_cast<std::uint32_t>(
                byte_value(bytes[position + 3])) << 24U);

        position += sizeof(std::uint32_t);
        return true;
    }

    [[nodiscard]] bool read_u64_le(
        std::span<const std::byte> bytes,
        std::size_t& position,
        std::uint64_t& value
    ) noexcept
    {
        if (position > bytes.size() ||
            bytes.size() - position < sizeof(std::uint64_t)) {
            return false;
        }

        value = 0;
        for (std::size_t i = 0; i < sizeof(std::uint64_t); ++i) {
            value |=
                static_cast<std::uint64_t>(
                    byte_value(bytes[position + i])) << (i * 8U);
        }

        position += sizeof(std::uint64_t);
        return true;
    }

    [[nodiscard]] Status validate_bytes(
        const void* pointer,
        std::uint32_t size,
        const char* field_name
    )
    {
        if (size > 0 && pointer == nullptr) {
            return Status{
                StatusCode::NullPointer,
                std::string(field_name) +
                    " pointer is null while its size is nonzero"
            };
        }

        return Status::ok();
    }

    [[nodiscard]] int compare_bytes(
        const void* lhs,
        std::uint32_t lhs_size,
        const void* rhs,
        std::uint32_t rhs_size
    ) noexcept
    {
        const std::size_t common =
            std::min<std::size_t>(lhs_size, rhs_size);

        if (common > 0) {
            const int compared = std::memcmp(lhs, rhs, common);
            if (compared < 0) {
                return -1;
            }
            if (compared > 0) {
                return 1;
            }
        }

        if (lhs_size < rhs_size) {
            return -1;
        }
        if (lhs_size > rhs_size) {
            return 1;
        }
        return 0;
    }

    [[nodiscard]] int compare_spans(
        std::span<const std::byte> lhs,
        std::span<const std::byte> rhs
    ) noexcept
    {
        return compare_bytes(
            lhs.data(),
            static_cast<std::uint32_t>(lhs.size()),
            rhs.data(),
            static_cast<std::uint32_t>(rhs.size())
        );
    }

    [[nodiscard]] Status validate_payload_sequence(
        const std::vector<IndexSection::Payload>& payloads,
        std::uint64_t& payload_bytes
    )
    {
        payload_bytes = 0;
        const IndexSection::Payload* previous = nullptr;

        for (const IndexSection::Payload& payload : payloads) {
            Status status = payload.validate();
            if (!status.is_ok()) {
                return status;
            }

            if (previous != nullptr) {
                std::uint64_t expected_offset = 0;
                if (!checked_add_u64(
                    previous->data_block_offset,
                    BLOCK_SIZE,
                    expected_offset)) {
                    return Status{
                        StatusCode::DataTypeOverflow,
                        "index data-block offset progression overflows uint64_t"
                    };
                }

                if (payload.data_block_offset != expected_offset) {
                    return Status{
                        StatusCode::InvalidSectionOffset,
                        "index entries must reference consecutive physical data blocks"
                    };
                }

                // Equality is intentional. One user key may span adjacent blocks.
                if (compare_bytes(
                    previous->last_key_ptr,
                    previous->last_key_size,
                    payload.first_key_ptr,
                    payload.first_key_size) > 0) {
                    return Status{
                        StatusCode::OverlappingKeys,
                        "adjacent index ranges overlap beyond one shared boundary key"
                    };
                }
            }

            const std::uint64_t encoded_size =
                static_cast<std::uint64_t>(
                    IndexSection::Payload::fixed_disk_size()) +
                payload.first_key_size +
                payload.last_key_size;

            if (!checked_add_u64(
                payload_bytes,
                encoded_size,
                payload_bytes)) {
                return Status{
                    StatusCode::DataTypeOverflow,
                    "index payload byte count overflows uint64_t"
                };
            }

            if (payload_bytes > kPayloadCapacity) {
                return Status{
                    StatusCode::InvalidSectionSize,
                    "version-1 index does not fit in its single physical block"
                };
            }

            previous = &payload;
        }

        return Status::ok();
    }

    [[nodiscard]] Result<IndexSection::Header> build_header(
        const std::vector<IndexSection::Payload>& payloads
    )
    {
        std::uint64_t payload_bytes = 0;
        Status status = validate_payload_sequence(payloads, payload_bytes);
        if (!status.is_ok()) {
            return Result<IndexSection::Header>::fail(std::move(status));
        }

        if (payload_bytes > std::numeric_limits<std::uint32_t>::max()) {
            return Result<IndexSection::Header>::fail(Status{
                StatusCode::DataTypeOverflow,
                "index payload size cannot be represented by uint32_t"
                });
        }

        IndexSection::Header result{};
        result.type = BlockType::Index;
        result.payload_size = static_cast<std::uint32_t>(payload_bytes);

        result.crc32 = static_cast<std::uint32_t>(
            ::crc32(0L, Z_NULL, 0)
            );
        for (const IndexSection::Payload& payload : payloads) {
            payload.append_crc32(result.crc32);
        }

        status = result.validate();
        if (!status.is_ok()) {
            return Result<IndexSection::Header>::fail(std::move(status));
        }

        return Result<IndexSection::Header>::ok(std::move(result));
    }

    [[nodiscard]] Status validate_parsed_sequence(
        const std::vector<std::byte>& encoded,
        const std::vector<ParsedPayload>& parsed
    )
    {
        const ParsedPayload* previous = nullptr;

        for (const ParsedPayload& payload : parsed) {
            const auto first = std::span<const std::byte>(
                encoded.data() + payload.first_key_position,
                payload.first_key_size
            );
            const auto last = std::span<const std::byte>(
                encoded.data() + payload.last_key_position,
                payload.last_key_size
            );

            if (payload.data_block_offset % BLOCK_SIZE != 0) {
                return Status{
                    StatusCode::InvalidSectionOffset,
                    "loaded index entry references an unaligned data block"
                };
            }

            if (compare_spans(first, last) > 0) {
                return Status{
                    StatusCode::Corruption,
                    "loaded index entry first key is greater than its last key"
                };
            }

            if (previous != nullptr) {
                std::uint64_t expected_offset = 0;
                if (!checked_add_u64(
                    previous->data_block_offset,
                    BLOCK_SIZE,
                    expected_offset)) {
                    return Status{
                        StatusCode::DataTypeOverflow,
                        "loaded index data-block offset progression overflows uint64_t"
                    };
                }

                if (payload.data_block_offset != expected_offset) {
                    return Status{
                        StatusCode::InvalidSectionOffset,
                        "loaded index entries do not reference consecutive data blocks"
                    };
                }

                const auto previous_last = std::span<const std::byte>(
                    encoded.data() + previous->last_key_position,
                    previous->last_key_size
                );

                if (compare_spans(previous_last, first) > 0) {
                    return Status{
                        StatusCode::OverlappingKeys,
                        "loaded adjacent index ranges overlap out of order"
                    };
                }
            }

            previous = &payload;
        }

        return Status::ok();
    }

    [[nodiscard]] Result<IndexSection> load_impl(
        ReadableFile& file,
        std::uint64_t& offset,
        Arena& arena,
        std::uint64_t index_offset,
        std::uint32_t expected_index_size,
        bool validate_layout,
        std::uint64_t first_data_block_offset,
        std::uint64_t data_block_count
    )
    {
        const std::uint64_t original_offset = offset;
        const Arena::Checkpoint checkpoint = arena.checkpoint();

        const auto fail = [&](Status status) {
            arena.rollback(checkpoint);
            offset = original_offset;
            return Result<IndexSection>::fail(std::move(status));
            };

        if (index_offset % BLOCK_SIZE != 0) {
            return fail(Status{
                StatusCode::InvalidBlockAlignment,
                "index section offset is not block aligned"
                });
        }

        std::uint64_t file_size = 0;
        Status status = file.get_file_size(file_size);
        if (!status.is_ok()) {
            return fail(std::move(status));
        }

        if (index_offset > file_size) {
            return fail(Status{
                StatusCode::OffsetOutOfRange,
                "index section offset lies beyond end of file"
                });
        }

        std::uint64_t cursor = index_offset;
        Result<IndexSection::Header> header_result =
            IndexSection::Header::load(file, cursor);

        if (!header_result.is_ok()) {
            return fail(std::move(header_result.status));
        }

        const IndexSection::Header loaded_header = header_result.value;

        std::uint64_t section_size = 0;
        if (!checked_add_u64(
            kHeaderSize,
            loaded_header.payload_size,
            section_size)) {
            return fail(Status{
                StatusCode::DataTypeOverflow,
                "index section size overflows uint64_t"
                });
        }

        if (expected_index_size != 0 &&
            section_size != expected_index_size) {
            return fail(Status{
                StatusCode::InvalidSectionSize,
                "footer index size does not match the index header"
                });
        }

        std::uint64_t section_end = 0;
        if (!checked_add_u64(index_offset, section_size, section_end) ||
            section_end > file_size) {
            return fail(Status{
                StatusCode::UnexpectedEOF,
                "index section is truncated"
                });
        }

        if (section_size > BLOCK_SIZE) {
            return fail(Status{
                StatusCode::InvalidSectionSize,
                "version-1 index section crosses its physical block"
                });
        }

        std::vector<std::byte> encoded_payload;
        try {
            encoded_payload.resize(loaded_header.payload_size);
        }
        catch (const std::bad_alloc&) {
            return fail(Status{
                StatusCode::BadAlloc,
                "failed to allocate temporary index payload buffer"
                });
        }

        if (!encoded_payload.empty()) {
            std::uint64_t payload_end = cursor;
            status = file.read_exact_at(
                cursor,
                encoded_payload.data(),
                encoded_payload.size(),
                payload_end
            );
            if (!status.is_ok()) {
                return fail(std::move(status));
            }

            if (payload_end != section_end) {
                return fail(Status{
                    StatusCode::InvalidOffset,
                    "index payload read ended at an unexpected offset"
                    });
            }

            cursor = payload_end;
        }

        const std::uint32_t calculated_crc = crc32_of(
            encoded_payload.data(),
            encoded_payload.size()
        );

        if (calculated_crc != loaded_header.crc32) {
            return fail(Status{
                StatusCode::ChecksumMismatch,
                std::format(
                    "index payload CRC mismatch: expected={}, actual={}",
                    calculated_crc,
                    loaded_header.crc32
                )
                });
        }

        std::vector<ParsedPayload> parsed;
        try {
            parsed.reserve(
                encoded_payload.size() /
                IndexSection::Payload::fixed_disk_size()
            );
        }
        catch (const std::bad_alloc&) {
            return fail(Status{
                StatusCode::BadAlloc,
                "failed to reserve parsed index entries"
                });
        }

        const std::span<const std::byte> bytes(encoded_payload);
        std::size_t position = 0;
        std::uint64_t total_key_bytes = 0;

        while (position < bytes.size()) {
            if (bytes.size() - position <
                IndexSection::Payload::fixed_disk_size()) {
                return fail(Status{
                    StatusCode::InvalidPayloadSize,
                    "index payload ends inside an entry's fixed fields"
                    });
            }

            ParsedPayload payload{};
            if (!read_u64_le(
                bytes,
                position,
                payload.data_block_offset) ||
                !read_u32_le(
                    bytes,
                    position,
                    payload.first_key_size) ||
                !read_u32_le(
                    bytes,
                    position,
                    payload.last_key_size)) {
                return fail(Status{
                    StatusCode::InvalidPayloadSize,
                    "failed to decode fixed index entry fields"
                    });
            }

            const std::uint64_t key_bytes =
                static_cast<std::uint64_t>(payload.first_key_size) +
                static_cast<std::uint64_t>(payload.last_key_size);

            if (key_bytes > bytes.size() - position) {
                return fail(Status{
                    StatusCode::InvalidPayloadSize,
                    "index entry key sizes exceed remaining payload bytes"
                    });
            }

            payload.first_key_position = position;
            position += payload.first_key_size;
            payload.last_key_position = position;
            position += payload.last_key_size;

            if (!checked_add_u64(
                total_key_bytes,
                key_bytes,
                total_key_bytes)) {
                return fail(Status{
                    StatusCode::DataTypeOverflow,
                    "loaded index key-byte count overflows uint64_t"
                    });
            }

            try {
                parsed.emplace_back(payload);
            }
            catch (const std::bad_alloc&) {
                return fail(Status{
                    StatusCode::BadAlloc,
                    "failed to append parsed index entry"
                    });
            }
        }

        status = validate_parsed_sequence(encoded_payload, parsed);
        if (!status.is_ok()) {
            return fail(std::move(status));
        }

        if (validate_layout) {
            std::uint64_t reserved_data_bytes = 0;
            std::uint64_t expected_index_offset = 0;

            if (!checked_mul_u64(
                data_block_count,
                BLOCK_SIZE,
                reserved_data_bytes) ||
                !checked_add_u64(
                    first_data_block_offset,
                    reserved_data_bytes,
                    expected_index_offset)) {
                return fail(Status{
                    StatusCode::DataTypeOverflow,
                    "footer data section extent overflows uint64_t"
                    });
            }

            if (data_block_count > 0 &&
                expected_index_offset != index_offset) {
                return fail(Status{
                    StatusCode::InvalidSectionOffset,
                    "footer data layout does not end at the index section"
                    });
            }

            if (data_block_count == 0 &&
                first_data_block_offset > index_offset) {
                return fail(Status{
                    StatusCode::InvalidSectionOffset,
                    "empty data section offset lies after the index section"
                    });
            }

            if (data_block_count >
                static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max()) ||
                parsed.size() !=
                static_cast<std::size_t>(data_block_count)) {
                return fail(Status{
                    StatusCode::InvalidSectionSize,
                    "index entry count does not match footer data-block count"
                    });
            }

            if (data_block_count > 0 &&
                first_data_block_offset % BLOCK_SIZE != 0) {
                return fail(Status{
                    StatusCode::InvalidBlockAlignment,
                    "footer data section offset is not block aligned"
                    });
            }

            for (std::uint64_t i = 0; i < data_block_count; ++i) {
                std::uint64_t displacement = 0;
                std::uint64_t expected_offset = 0;

                if (!checked_mul_u64(i, BLOCK_SIZE, displacement) ||
                    !checked_add_u64(
                        first_data_block_offset,
                        displacement,
                        expected_offset)) {
                    return fail(Status{
                        StatusCode::DataTypeOverflow,
                        "footer data layout overflows uint64_t"
                        });
                }

                if (parsed[static_cast<std::size_t>(i)].data_block_offset !=
                    expected_offset) {
                    return fail(Status{
                        StatusCode::InvalidSectionOffset,
                        "index data-block offset does not match footer data layout"
                        });
                }
            }
        }

        if (total_key_bytes >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
            return fail(Status{
                StatusCode::AllocationTooLarge,
                "loaded index keys cannot be represented by size_t"
                });
        }

        IndexSection result{};
        result.header = loaded_header;

        try {
            result.payloads.reserve(parsed.size());
        }
        catch (const std::bad_alloc&) {
            return fail(Status{
                StatusCode::BadAlloc,
                "failed to reserve loaded index entries"
                });
        }

        std::byte* key_storage = nullptr;
        if (total_key_bytes > 0) {
            Result<void*> allocation = arena.alloc(
                static_cast<std::size_t>(total_key_bytes),
                alignof(std::byte)
            );
            if (!allocation.is_ok()) {
                return fail(std::move(allocation.status));
            }

            key_storage = static_cast<std::byte*>(allocation.value);
        }

        std::size_t destination_position = 0;
        for (const ParsedPayload& parsed_payload : parsed) {
            IndexSection::Payload payload{};
            payload.data_block_offset = parsed_payload.data_block_offset;
            payload.first_key_size = parsed_payload.first_key_size;
            payload.last_key_size = parsed_payload.last_key_size;

            if (payload.first_key_size > 0) {
                payload.first_key_ptr =
                    key_storage + destination_position;
                std::memcpy(
                    key_storage + destination_position,
                    encoded_payload.data() +
                    parsed_payload.first_key_position,
                    payload.first_key_size
                );
                destination_position += payload.first_key_size;
            }

            if (payload.last_key_size > 0) {
                payload.last_key_ptr =
                    key_storage + destination_position;
                std::memcpy(
                    key_storage + destination_position,
                    encoded_payload.data() +
                    parsed_payload.last_key_position,
                    payload.last_key_size
                );
                destination_position += payload.last_key_size;
            }

            try {
                result.payloads.emplace_back(payload);
            }
            catch (const std::bad_alloc&) {
                return fail(Status{
                    StatusCode::BadAlloc,
                    "failed to append loaded index entry"
                    });
            }
        }

        status = result.validate();
        if (!status.is_ok()) {
            return fail(std::move(status));
        }

        offset = section_end;
        return Result<IndexSection>::ok(std::move(result));
    }
}

IndexSection::IndexSection() noexcept
{
    header.type = BlockType::Index;
    header.payload_size = 0;
    header.crc32 = static_cast<std::uint32_t>(
        ::crc32(0L, Z_NULL, 0)
        );
}

Status IndexSection::Header::validate() const
{
    if (type != BlockType::Index) {
        return Status{
            StatusCode::InvalidBlockType,
            "index header has a non-index block type"
        };
    }

    if (payload_size > kPayloadCapacity) {
        return Status{
            StatusCode::InvalidPayloadSize,
            "version-1 index payload exceeds its single-block capacity"
        };
    }

    return Status::ok();
}

Status IndexSection::Header::write(
    WritableFile& file,
    std::uint64_t& offset
) const
{
    Status status = validate();
    if (!status.is_ok()) {
        return status;
    }

    Result<std::uint64_t> position = file.current_position();
    if (!position.is_ok()) {
        return std::move(position.status);
    }

    if (position.value != offset) {
        return Status{
            StatusCode::InvalidOffset,
            "index header tracked offset does not match file cursor"
        };
    }

    status = fits_in_block(offset, disk_size(), BLOCK_SIZE);
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
        payload_size,
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

Result<IndexSection::Header> IndexSection::Header::load(
    ReadableFile& file,
    std::uint64_t& offset
)
{
    const std::uint64_t original_offset = offset;
    std::uint64_t cursor = offset;

    Status status = fits_in_block(cursor, disk_size(), BLOCK_SIZE);
    if (!status.is_ok()) {
        return Result<Header>::fail(std::move(status));
    }

    Header result{};
    std::uint8_t encoded_type = 0;

    status = kvdb::blockio::read_u8_t(
        file,
        encoded_type,
        cursor,
        BLOCK_SIZE
    );
    if (!status.is_ok()) {
        offset = original_offset;
        return Result<Header>::fail(std::move(status));
    }

    status = kvdb::blockio::read_u32_t_le(
        file,
        result.payload_size,
        cursor,
        BLOCK_SIZE
    );
    if (!status.is_ok()) {
        offset = original_offset;
        return Result<Header>::fail(std::move(status));
    }

    status = kvdb::blockio::read_u32_t_le(
        file,
        result.crc32,
        cursor,
        BLOCK_SIZE
    );
    if (!status.is_ok()) {
        offset = original_offset;
        return Result<Header>::fail(std::move(status));
    }

    result.type = static_cast<BlockType>(encoded_type);
    status = result.validate();
    if (!status.is_ok()) {
        offset = original_offset;
        return Result<Header>::fail(std::move(status));
    }

    offset = cursor;
    return Result<Header>::ok(std::move(result));
}

std::size_t IndexSection::Payload::disk_size() const noexcept
{
    return fixed_disk_size() +
        static_cast<std::size_t>(first_key_size) +
        static_cast<std::size_t>(last_key_size);
}

std::span<const std::byte> IndexSection::Payload::first_key() const noexcept
{
    return std::span<const std::byte>(
        static_cast<const std::byte*>(first_key_ptr),
        first_key_size
    );
}

std::span<const std::byte> IndexSection::Payload::last_key() const noexcept
{
    return std::span<const std::byte>(
        static_cast<const std::byte*>(last_key_ptr),
        last_key_size
    );
}

Status IndexSection::Payload::validate() const
{
    if (data_block_offset % BLOCK_SIZE != 0) {
        return Status{
            StatusCode::InvalidSectionOffset,
            "index entry data-block offset is not block aligned"
        };
    }

    Status status = validate_bytes(
        first_key_ptr,
        first_key_size,
        "index first key"
    );
    if (!status.is_ok()) {
        return status;
    }

    status = validate_bytes(
        last_key_ptr,
        last_key_size,
        "index last key"
    );
    if (!status.is_ok()) {
        return status;
    }

    const std::uint64_t encoded_size =
        static_cast<std::uint64_t>(fixed_disk_size()) +
        first_key_size +
        last_key_size;

    if (encoded_size > kPayloadCapacity) {
        return Status{
            StatusCode::InvalidPayloadSize,
            "one index entry cannot fit beside the index header"
        };
    }

    if (compare_bytes(
        first_key_ptr,
        first_key_size,
        last_key_ptr,
        last_key_size) > 0) {
        return Status{
            StatusCode::InvalidArgument,
            "index entry first key is greater than its last key"
        };
    }

    return Status::ok();
}

Status IndexSection::Payload::write(
    WritableFile& file,
    std::uint64_t& offset
) const
{
    Status status = validate();
    if (!status.is_ok()) {
        return status;
    }

    Result<std::uint64_t> position = file.current_position();
    if (!position.is_ok()) {
        return std::move(position.status);
    }

    if (position.value != offset) {
        return Status{
            StatusCode::InvalidOffset,
            "index payload tracked offset does not match file cursor"
        };
    }

    // One entry is indivisible. Never let block I/O align between its fields.
    status = fits_in_block(offset, disk_size(), BLOCK_SIZE);
    if (!status.is_ok()) {
        return status;
    }

    status = kvdb::blockio::write_u64_t_le(
        file,
        data_block_offset,
        offset,
        BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u32_t_le(
        file,
        first_key_size,
        offset,
        BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u32_t_le(
        file,
        last_key_size,
        offset,
        BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_bytes(
        file,
        first_key(),
        offset,
        BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    return kvdb::blockio::write_bytes(
        file,
        last_key(),
        offset,
        BLOCK_SIZE
    );
}

void IndexSection::Payload::append_crc32(
    std::uint32_t& crc_buffer
) const noexcept
{
    crc32_add_pod<std::uint64_t>(crc_buffer, data_block_offset);
    crc32_add_pod<std::uint32_t>(crc_buffer, first_key_size);
    crc32_add_pod<std::uint32_t>(crc_buffer, last_key_size);

    if (first_key_size > 0) {
        compute_crc32(crc_buffer, first_key_ptr, first_key_size);
    }
    if (last_key_size > 0) {
        compute_crc32(crc_buffer, last_key_ptr, last_key_size);
    }
}

void IndexSection::Payload::calculate_crc32(
    std::uint32_t& crc_buffer
) const noexcept
{
    crc_buffer = static_cast<std::uint32_t>(
        ::crc32(0L, Z_NULL, 0)
        );
    append_crc32(crc_buffer);
}

std::size_t IndexSection::disk_size() const noexcept
{
    std::size_t result = Header::disk_size();
    for (const Payload& payload : payloads) {
        result += payload.disk_size();
    }
    return result;
}

Status IndexSection::validate() const
{
    std::uint64_t payload_bytes = 0;
    Status status = validate_payload_sequence(payloads, payload_bytes);
    if (!status.is_ok()) {
        return status;
    }

    status = header.validate();
    if (!status.is_ok()) {
        return status;
    }

    if (header.payload_size != payload_bytes) {
        return Status{
            StatusCode::InvalidHeader,
            "index header payload size is stale or inconsistent"
        };
    }

    std::uint32_t expected_crc = static_cast<std::uint32_t>(
        ::crc32(0L, Z_NULL, 0)
        );
    for (const Payload& payload : payloads) {
        payload.append_crc32(expected_crc);
    }

    if (header.crc32 != expected_crc) {
        return Status{
            StatusCode::InvalidHeader,
            "index header CRC is stale or inconsistent"
        };
    }

    return Status::ok();
}

Status IndexSection::rebuild_header()
{
    Result<Header> rebuilt = build_header(payloads);
    if (!rebuilt.is_ok()) {
        return std::move(rebuilt.status);
    }

    header = rebuilt.value;
    return Status::ok();
}

Status IndexSection::validate_data_layout(
    std::uint64_t first_data_block_offset,
    std::uint64_t data_block_count
) const
{
    if (data_block_count >
        static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max()) ||
        payloads.size() !=
        static_cast<std::size_t>(data_block_count)) {
        return Status{
            StatusCode::InvalidSectionSize,
            "index entry count does not match data-block count"
        };
    }

    if (data_block_count == 0) {
        return Status::ok();
    }

    if (first_data_block_offset % BLOCK_SIZE != 0) {
        return Status{
            StatusCode::InvalidBlockAlignment,
            "first data-block offset is not block aligned"
        };
    }

    for (std::uint64_t i = 0; i < data_block_count; ++i) {
        std::uint64_t displacement = 0;
        std::uint64_t expected_offset = 0;

        if (!checked_mul_u64(i, BLOCK_SIZE, displacement) ||
            !checked_add_u64(
                first_data_block_offset,
                displacement,
                expected_offset)) {
            return Status{
                StatusCode::DataTypeOverflow,
                "expected data-block layout overflows uint64_t"
            };
        }

        if (payloads[static_cast<std::size_t>(i)].data_block_offset !=
            expected_offset) {
            return Status{
                StatusCode::InvalidSectionOffset,
                "index entry points outside the expected data-block layout"
            };
        }
    }

    return Status::ok();
}

Status IndexSection::add_index(
    std::uint64_t data_block_offset,
    std::uint32_t first_key_size,
    std::uint32_t last_key_size,
    const void* first_key_ptr,
    const void* last_key_ptr
)
{
    std::uint64_t existing_payload_bytes = 0;
    Status status = validate_payload_sequence(
        payloads,
        existing_payload_bytes
    );
    if (!status.is_ok()) {
        return status;
    }

    Payload payload{};
    payload.data_block_offset = data_block_offset;
    payload.first_key_size = first_key_size;
    payload.last_key_size = last_key_size;
    payload.first_key_ptr = first_key_ptr;
    payload.last_key_ptr = last_key_ptr;

    status = payload.validate();
    if (!status.is_ok()) {
        return status;
    }

    if (!payloads.empty()) {
        const Payload& previous = payloads.back();

        std::uint64_t expected_offset = 0;
        if (!checked_add_u64(
            previous.data_block_offset,
            BLOCK_SIZE,
            expected_offset)) {
            return Status{
                StatusCode::DataTypeOverflow,
                "new index data-block offset progression overflows uint64_t"
            };
        }

        if (payload.data_block_offset != expected_offset) {
            return Status{
                StatusCode::InvalidSectionOffset,
                "new index entry must reference the next physical data block"
            };
        }

        if (compare_bytes(
            previous.last_key_ptr,
            previous.last_key_size,
            payload.first_key_ptr,
            payload.first_key_size) > 0) {
            return Status{
                StatusCode::OverlappingKeys,
                "new index key range is out of global key order"
            };
        }
    }

    std::uint64_t new_payload_bytes = 0;
    if (!checked_add_u64(
        existing_payload_bytes,
        static_cast<std::uint64_t>(Payload::fixed_disk_size()) +
        payload.first_key_size +
        payload.last_key_size,
        new_payload_bytes) ||
        new_payload_bytes > kPayloadCapacity) {
        return Status{
            StatusCode::InvalidSectionSize,
            "new index entry would exceed the version-1 single-block limit"
        };
    }

    const Header old_header = header;

    try {
        payloads.emplace_back(payload);
    }
    catch (const std::bad_alloc&) {
        return Status{
            StatusCode::BadAlloc,
            "failed to append index entry"
        };
    }

    status = rebuild_header();
    if (!status.is_ok()) {
        payloads.pop_back();
        header = old_header;
        return status;
    }

    return Status::ok();
}

Status IndexSection::write(
    WritableFile& file,
    std::uint64_t& offset,
    std::uint64_t& index_offset
)
{
    Result<Header> staged_header = build_header(payloads);
    if (!staged_header.is_ok()) {
        return std::move(staged_header.status);
    }

    Result<std::uint64_t> position = file.current_position();
    if (!position.is_ok()) {
        return std::move(position.status);
    }

    if (position.value != offset) {
        return Status{
            StatusCode::InvalidOffset,
            "index section tracked offset does not match file cursor"
        };
    }

    Status status = align_to_block_boundary(file, offset, BLOCK_SIZE);
    if (!status.is_ok()) {
        return status;
    }

    const std::uint64_t staged_index_offset = offset;
    const std::size_t section_size =
        Header::disk_size() + staged_header.value.payload_size;

    status = fits_in_block(offset, section_size, BLOCK_SIZE);
    if (!status.is_ok()) {
        return status;
    }

    status = staged_header.value.write(file, offset);
    if (!status.is_ok()) {
        return status;
    }

    for (const Payload& payload : payloads) {
        status = payload.write(file, offset);
        if (!status.is_ok()) {
            return status;
        }
    }

    header = staged_header.value;
    index_offset = staged_index_offset;
    return Status::ok();
}

Result<IndexSection> IndexSection::load(
    ReadableFile& file,
    std::uint64_t& offset,
    Arena& arena,
    std::uint64_t index_offset,
    std::uint32_t expected_index_size
)
{
    return load_impl(
        file,
        offset,
        arena,
        index_offset,
        expected_index_size,
        false,
        0,
        0
    );
}

Result<IndexSection> IndexSection::load(
    ReadableFile& file,
    std::uint64_t& offset,
    Arena& arena,
    std::uint64_t index_offset,
    std::uint32_t expected_index_size,
    std::uint64_t first_data_block_offset,
    std::uint64_t data_block_count
)
{
    return load_impl(
        file,
        offset,
        arena,
        index_offset,
        expected_index_size,
        true,
        first_data_block_offset,
        data_block_count
    );
}

Result<IndexSection::CandidateRange>
IndexSection::find_candidate_range(
    const void* key_ptr,
    std::uint32_t key_size
) const
{
    Status status = validate_bytes(key_ptr, key_size, "lookup key");
    if (!status.is_ok()) {
        return Result<CandidateRange>::fail(std::move(status));
    }

    std::uint64_t ignored_payload_bytes = 0;
    status = validate_payload_sequence(payloads, ignored_payload_bytes);
    if (!status.is_ok()) {
        return Result<CandidateRange>::fail(std::move(status));
    }

    std::size_t low = 0;
    std::size_t high = payloads.size();

    // First range whose last key is >= the lookup key.
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        const Payload& payload = payloads[middle];

        if (compare_bytes(
            payload.last_key_ptr,
            payload.last_key_size,
            key_ptr,
            key_size) < 0) {
            low = middle + 1;
        }
        else {
            high = middle;
        }
    }

    const std::size_t first = low;
    if (first == payloads.size() ||
        compare_bytes(
            payloads[first].first_key_ptr,
            payloads[first].first_key_size,
            key_ptr,
            key_size) > 0) {
        return Result<CandidateRange>::fail(Status{
            StatusCode::NotFound,
            "lookup key is outside every index range"
            });
    }

    low = first;
    high = payloads.size();

    // First range whose first key is > the lookup key.
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        const Payload& payload = payloads[middle];

        if (compare_bytes(
            payload.first_key_ptr,
            payload.first_key_size,
            key_ptr,
            key_size) <= 0) {
            low = middle + 1;
        }
        else {
            high = middle;
        }
    }

    return Result<CandidateRange>::ok(CandidateRange{
        first,
        low
        });
}

Result<std::size_t> IndexSection::find_first_candidate(
    const void* key_ptr,
    std::uint32_t key_size
) const
{
    Result<CandidateRange> range =
        find_candidate_range(key_ptr, key_size);

    if (!range.is_ok()) {
        return Result<std::size_t>::fail(std::move(range.status));
    }

    return Result<std::size_t>::ok(range.value.first);
}

void IndexSection::calculate_crc32(
    std::uint32_t& crc_buffer
) const noexcept
{
    crc_buffer = static_cast<std::uint32_t>(
        ::crc32(0L, Z_NULL, 0)
        );

    for (const Payload& payload : payloads) {
        payload.append_crc32(crc_buffer);
    }
}