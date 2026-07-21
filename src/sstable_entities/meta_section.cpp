#include "sstable_entities/meta_section.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

using namespace SSTableEntities;

namespace
{
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
                    " pointer is null while its encoded size is nonzero"
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
        const std::size_t common = std::min<std::size_t>(lhs_size, rhs_size);
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

    [[nodiscard]] bool equal_bytes(
        const void* lhs,
        std::uint32_t lhs_size,
        const void* rhs,
        std::uint32_t rhs_size
    ) noexcept
    {
        return compare_bytes(lhs, lhs_size, rhs, rhs_size) == 0;
    }

    [[nodiscard]] bool add_overflows_u64(
        std::uint64_t lhs,
        std::uint64_t rhs
    ) noexcept
    {
        return lhs > std::numeric_limits<std::uint64_t>::max() - rhs;
    }

    [[nodiscard]] bool multiply_overflows_u64(
        std::uint64_t lhs,
        std::uint64_t rhs
    ) noexcept
    {
        return lhs != 0 &&
            rhs > std::numeric_limits<std::uint64_t>::max() / lhs;
    }

    [[nodiscard]] Status validate_index_consistency(
        const MetaSection::Payload& payload,
        const IndexSection& index_section
    )
    {
        if (payload.data_block_count != index_section.payloads.size()) {
            return Status{
                StatusCode::InvariantViolation,
                "meta data-block count does not match index entry count"
            };
        }

        if (payload.record_count == 0) {
            if (!index_section.payloads.empty()) {
                return Status{
                    StatusCode::InvariantViolation,
                    "empty metadata cannot reference nonempty index entries"
                };
            }
            return Status::ok();
        }

        if (index_section.payloads.empty()) {
            return Status{
                StatusCode::InvariantViolation,
                "nonempty metadata requires at least one index entry"
            };
        }

        const void* observed_min_ptr = nullptr;
        std::uint32_t observed_min_size = 0;
        const void* observed_max_ptr = nullptr;
        std::uint32_t observed_max_size = 0;
        bool have_boundary = false;

        const auto observe = [&](const void* pointer, std::uint32_t size) {
            if (!have_boundary) {
                observed_min_ptr = pointer;
                observed_min_size = size;
                observed_max_ptr = pointer;
                observed_max_size = size;
                have_boundary = true;
                return;
            }

            if (compare_bytes(
                pointer,
                size,
                observed_min_ptr,
                observed_min_size
            ) < 0) {
                observed_min_ptr = pointer;
                observed_min_size = size;
            }

            if (compare_bytes(
                pointer,
                size,
                observed_max_ptr,
                observed_max_size
            ) > 0) {
                observed_max_ptr = pointer;
                observed_max_size = size;
            }
            };

        for (const IndexSection::Payload& entry : index_section.payloads) {
            Status status = validate_bytes(
                entry.first_key_ptr,
                entry.first_key_size,
                "index first key"
            );
            if (!status.is_ok()) {
                return status;
            }

            status = validate_bytes(
                entry.last_key_ptr,
                entry.last_key_size,
                "index last key"
            );
            if (!status.is_ok()) {
                return status;
            }

            observe(entry.first_key_ptr, entry.first_key_size);
            observe(entry.last_key_ptr, entry.last_key_size);
        }

        if (!equal_bytes(
            payload.min_key_ptr,
            payload.min_key_size,
            observed_min_ptr,
            observed_min_size
        )) {
            return Status{
                StatusCode::InvariantViolation,
                "meta minimum key does not match index key boundaries"
            };
        }

        if (!equal_bytes(
            payload.max_key_ptr,
            payload.max_key_size,
            observed_max_ptr,
            observed_max_size
        )) {
            return Status{
                StatusCode::InvariantViolation,
                "meta maximum key does not match index key boundaries"
            };
        }

        return Status::ok();
    }

    [[nodiscard]] Result<MetaSection::Header> build_header(
        const MetaSection::Payload& payload
    )
    {
        Status status = payload.validate();
        if (!status.is_ok()) {
            return Result<MetaSection::Header>::fail(std::move(status));
        }

        const std::size_t payload_size = payload.disk_size();
        if (payload_size > std::numeric_limits<std::uint32_t>::max()) {
            return Result<MetaSection::Header>::fail(Status{
                StatusCode::DataTypeOverflow,
                "meta payload size cannot be represented by u32"
                });
        }

        MetaSection::Header result{};
        result.type = BlockType::Meta;
        result.payload_size = static_cast<std::uint32_t>(payload_size);
        payload.calculate_crc32(result.crc32);

        status = result.validate();
        if (!status.is_ok()) {
            return Result<MetaSection::Header>::fail(std::move(status));
        }

        return Result<MetaSection::Header>::ok(std::move(result));
    }
}

Status MetaSection::Header::validate() const
{
    if (type != BlockType::Meta) {
        return Status{
            StatusCode::InvalidBlockType,
            "meta header has an invalid block type"
        };
    }

    if (payload_size < Payload::fixed_disk_size()) {
        return Status{
            StatusCode::InvalidPayloadSize,
            "meta payload is smaller than its fixed fields"
        };
    }

    if (payload_size > BLOCK_SIZE - disk_size()) {
        return Status{
            StatusCode::InvalidPayloadSize,
            "meta payload does not fit beside its header in one block"
        };
    }

    return Status::ok();
}

Status MetaSection::Header::write(
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
            "meta header tracked offset does not match file cursor"
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

Result<MetaSection::Header> MetaSection::Header::load(
    ReadableFile& file,
    std::uint64_t& offset
)
{
    const std::uint64_t initial_offset = offset;
    Header result{};
    std::uint8_t encoded_type = 0;

    Status status = fits_in_block(offset, disk_size(), BLOCK_SIZE);
    if (!status.is_ok()) {
        return Result<Header>::fail(std::move(status));
    }

    status = kvdb::blockio::read_u8_t(
        file,
        encoded_type,
        offset,
        BLOCK_SIZE
    );
    if (!status.is_ok()) {
        offset = initial_offset;
        return Result<Header>::fail(std::move(status));
    }

    status = kvdb::blockio::read_u32_t_le(
        file,
        result.payload_size,
        offset,
        BLOCK_SIZE
    );
    if (!status.is_ok()) {
        offset = initial_offset;
        return Result<Header>::fail(std::move(status));
    }

    status = kvdb::blockio::read_u32_t_le(
        file,
        result.crc32,
        offset,
        BLOCK_SIZE
    );
    if (!status.is_ok()) {
        offset = initial_offset;
        return Result<Header>::fail(std::move(status));
    }

    result.type = static_cast<BlockType>(encoded_type);
    status = result.validate();
    if (!status.is_ok()) {
        offset = initial_offset;
        return Result<Header>::fail(std::move(status));
    }

    return Result<Header>::ok(std::move(result));
}

std::size_t MetaSection::Payload::disk_size() const noexcept
{
    return fixed_disk_size() +
        static_cast<std::size_t>(min_key_size) +
        static_cast<std::size_t>(max_key_size);
}

Status MetaSection::Payload::validate() const
{
    Status status = validate_bytes(min_key_ptr, min_key_size, "meta minimum key");
    if (!status.is_ok()) {
        return status;
    }

    status = validate_bytes(max_key_ptr, max_key_size, "meta maximum key");
    if (!status.is_ok()) {
        return status;
    }

    const std::uint64_t encoded_size =
        static_cast<std::uint64_t>(fixed_disk_size()) +
        min_key_size +
        max_key_size;

    if (encoded_size > BLOCK_SIZE - Header::disk_size()) {
        return Status{
            StatusCode::InvalidPayloadSize,
            "meta payload and both boundary keys do not fit in one block"
        };
    }

    if (tombstone_count > record_count) {
        return Status{
            StatusCode::InvalidPayloadSize,
            "meta tombstone count exceeds record count"
        };
    }

    if (record_count == 0) {
        if (tombstone_count != 0 ||
            min_seq_num != 0 ||
            max_seq_num != 0 ||
            min_key_size != 0 ||
            max_key_size != 0 ||
            data_block_count != 0 ||
            data_bytes != 0) {
            return Status{
                StatusCode::InvalidPayloadSize,
                "empty meta payload is not in canonical zero state"
            };
        }

        return Status::ok();
    }

    if (data_block_count == 0) {
        return Status{
            StatusCode::InvalidPayloadSize,
            "nonempty meta payload has zero data blocks"
        };
    }

    if (data_block_count > record_count) {
        return Status{
            StatusCode::InvalidPayloadSize,
            "meta data-block count exceeds record count"
        };
    }

    if (min_seq_num > max_seq_num) {
        return Status{
            StatusCode::InvalidPayloadSize,
            "meta minimum sequence number exceeds maximum sequence number"
        };
    }

    if (compare_bytes(
        min_key_ptr,
        min_key_size,
        max_key_ptr,
        max_key_size
    ) > 0) {
        return Status{
            StatusCode::InvalidPayloadSize,
            "meta minimum key is greater than maximum key"
        };
    }

    const std::uint64_t minimum_block_bytes =
        DataSection::Header::disk_size() +
        DataSection::Payload::fixed_part_disk_size();

    if (multiply_overflows_u64(data_block_count, minimum_block_bytes) ||
        multiply_overflows_u64(data_block_count, BLOCK_SIZE)) {
        return Status{
            StatusCode::DataTypeOverflow,
            "meta data-byte bounds overflow uint64_t"
        };
    }

    const std::uint64_t minimum_data_bytes =
        data_block_count * minimum_block_bytes;
    const std::uint64_t maximum_data_bytes =
        data_block_count * BLOCK_SIZE;

    if (data_bytes < minimum_data_bytes || data_bytes > maximum_data_bytes) {
        return Status{
            StatusCode::InvalidPayloadSize,
            "meta data-byte count is impossible for its data-block count"
        };
    }

    return Status::ok();
}

Status MetaSection::Payload::write(
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
            "meta payload tracked offset does not match file cursor"
        };
    }

    status = fits_in_block(offset, disk_size(), BLOCK_SIZE);
    if (!status.is_ok()) {
        return status;
    }

    status = kvdb::blockio::write_u64_t_le(file, record_count, offset, BLOCK_SIZE);
    if (!status.is_ok()) return status;
    status = kvdb::blockio::write_u64_t_le(file, tombstone_count, offset, BLOCK_SIZE);
    if (!status.is_ok()) return status;
    status = kvdb::blockio::write_u64_t_le(file, min_seq_num, offset, BLOCK_SIZE);
    if (!status.is_ok()) return status;
    status = kvdb::blockio::write_u64_t_le(file, max_seq_num, offset, BLOCK_SIZE);
    if (!status.is_ok()) return status;
    status = kvdb::blockio::write_u32_t_le(file, min_key_size, offset, BLOCK_SIZE);
    if (!status.is_ok()) return status;
    status = kvdb::blockio::write_u32_t_le(file, max_key_size, offset, BLOCK_SIZE);
    if (!status.is_ok()) return status;
    status = kvdb::blockio::write_u64_t_le(file, data_block_count, offset, BLOCK_SIZE);
    if (!status.is_ok()) return status;
    status = kvdb::blockio::write_u64_t_le(file, data_bytes, offset, BLOCK_SIZE);
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_bytes(
        file,
        std::span<const std::byte>(
            static_cast<const std::byte*>(min_key_ptr),
            min_key_size
        ),
        offset,
        BLOCK_SIZE
    );
    if (!status.is_ok()) {
        return status;
    }

    return kvdb::blockio::write_bytes(
        file,
        std::span<const std::byte>(
            static_cast<const std::byte*>(max_key_ptr),
            max_key_size
        ),
        offset,
        BLOCK_SIZE
    );
}

Result<MetaSection::Payload> MetaSection::Payload::load(
    ReadableFile& file,
    std::uint64_t& offset,
    Arena& arena,
    const Header& header
)
{
    const std::uint64_t initial_offset = offset;
    const Arena::Checkpoint checkpoint = arena.checkpoint();

    const auto fail = [&](Status status) -> Result<Payload> {
        offset = initial_offset;
        arena.rollback(checkpoint);
        return Result<Payload>::fail(std::move(status));
        };

    Status status = header.validate();
    if (!status.is_ok()) {
        return fail(std::move(status));
    }

    status = fits_in_block(offset, header.payload_size, BLOCK_SIZE);
    if (!status.is_ok()) {
        return fail(std::move(status));
    }

    Payload result{};

    status = kvdb::blockio::read_u64_t_le(file, result.record_count, offset, BLOCK_SIZE);
    if (!status.is_ok()) return fail(std::move(status));
    status = kvdb::blockio::read_u64_t_le(file, result.tombstone_count, offset, BLOCK_SIZE);
    if (!status.is_ok()) return fail(std::move(status));
    status = kvdb::blockio::read_u64_t_le(file, result.min_seq_num, offset, BLOCK_SIZE);
    if (!status.is_ok()) return fail(std::move(status));
    status = kvdb::blockio::read_u64_t_le(file, result.max_seq_num, offset, BLOCK_SIZE);
    if (!status.is_ok()) return fail(std::move(status));
    status = kvdb::blockio::read_u32_t_le(file, result.min_key_size, offset, BLOCK_SIZE);
    if (!status.is_ok()) return fail(std::move(status));
    status = kvdb::blockio::read_u32_t_le(file, result.max_key_size, offset, BLOCK_SIZE);
    if (!status.is_ok()) return fail(std::move(status));
    status = kvdb::blockio::read_u64_t_le(file, result.data_block_count, offset, BLOCK_SIZE);
    if (!status.is_ok()) return fail(std::move(status));
    status = kvdb::blockio::read_u64_t_le(file, result.data_bytes, offset, BLOCK_SIZE);
    if (!status.is_ok()) return fail(std::move(status));

    const std::uint64_t expected_size =
        static_cast<std::uint64_t>(fixed_disk_size()) +
        result.min_key_size +
        result.max_key_size;

    if (expected_size != header.payload_size) {
        return fail(Status{
            StatusCode::InvalidPayloadSize,
            "meta payload size does not match its encoded key lengths"
            });
    }

    const std::uint64_t key_bytes_u64 =
        static_cast<std::uint64_t>(result.min_key_size) +
        result.max_key_size;
    if (key_bytes_u64 > std::numeric_limits<std::size_t>::max()) {
        return fail(Status{
            StatusCode::AllocationTooLarge,
            "meta boundary keys do not fit size_t"
            });
    }

    std::byte* key_storage = nullptr;
    const std::size_t key_bytes = static_cast<std::size_t>(key_bytes_u64);
    if (key_bytes > 0) {
        Result<void*> allocation = arena.alloc(key_bytes, alignof(std::byte));
        if (!allocation.is_ok()) {
            return fail(std::move(allocation.status));
        }
        key_storage = static_cast<std::byte*>(allocation.value);
    }

    if (result.min_key_size > 0) {
        result.min_key_ptr = key_storage;
        status = kvdb::blockio::read_bytes(
            file,
            key_storage,
            result.min_key_size,
            offset,
            BLOCK_SIZE
        );
        if (!status.is_ok()) return fail(std::move(status));
    }

    if (result.max_key_size > 0) {
        result.max_key_ptr = key_storage + result.min_key_size;
        status = kvdb::blockio::read_bytes(
            file,
            key_storage + result.min_key_size,
            result.max_key_size,
            offset,
            BLOCK_SIZE
        );
        if (!status.is_ok()) return fail(std::move(status));
    }

    std::uint32_t calculated_crc = 0;
    result.calculate_crc32(calculated_crc);
    if (calculated_crc != header.crc32) {
        return fail(Status{
            StatusCode::ChecksumMismatch,
            "meta payload CRC mismatch"
            });
    }

    status = result.validate();
    if (!status.is_ok()) {
        return fail(std::move(status));
    }

    return Result<Payload>::ok(std::move(result));
}

MetaSection::MetaSection() noexcept
{
    payload.calculate_crc32(header.crc32);
    header.type = BlockType::Meta;
    header.payload_size = static_cast<std::uint32_t>(payload.disk_size());
}

std::size_t MetaSection::disk_size() const noexcept
{
    return Header::disk_size() + payload.disk_size();
}

Status MetaSection::rebuild(
    const DataSection& data_section,
    const IndexSection& index_section
)
{
    if (data_section.data_blocks.size() != index_section.payloads.size()) {
        return Status{
            StatusCode::InvariantViolation,
            "cannot rebuild meta: data-block and index-entry counts differ"
        };
    }

    if (data_section.data_blocks.size() >
        std::numeric_limits<std::uint64_t>::max()) {
        return Status{
            StatusCode::DataTypeOverflow,
            "data-block count cannot be represented by u64"
        };
    }

    Payload staged{};
    staged.data_block_count =
        static_cast<std::uint64_t>(data_section.data_blocks.size());

    bool have_record = false;

    for (const DataSection::DataBlock& block : data_section.data_blocks) {
        if (block.payloads.empty()) {
            return Status{
                StatusCode::InvalidState,
                "cannot rebuild meta from an empty data block"
            };
        }

        const std::size_t block_size = block.disk_size();
        if (block_size > BLOCK_SIZE) {
            return Status{
                StatusCode::InvalidPayloadSize,
                "cannot rebuild meta from an oversized data block"
            };
        }

        if (block_size > std::numeric_limits<std::uint64_t>::max() ||
            add_overflows_u64(
                staged.data_bytes,
                static_cast<std::uint64_t>(block_size)
            )) {
            return Status{
                StatusCode::DataTypeOverflow,
                "meta data-byte count overflowed u64"
            };
        }
        staged.data_bytes += static_cast<std::uint64_t>(block_size);

        for (const DataSection::Payload& record : block.payloads) {
            Status status = validate_bytes(
                record.key_ptr,
                record.key_size,
                "data record key"
            );
            if (!status.is_ok()) {
                return status;
            }

            status = validate_bytes(
                record.value_ptr,
                record.value_size,
                "data record value"
            );
            if (!status.is_ok()) {
                return status;
            }

            if (record.type != Type::Put && record.type != Type::Tombstone) {
                return Status{
                    StatusCode::InvalidArgument,
                    "cannot rebuild meta from an invalid record type"
                };
            }

            if (record.flags != 0 || record.reserved != 0) {
                return Status{
                    StatusCode::InvalidArgument,
                    "version-1 data records require zero flags and reserved fields"
                };
            }

            if (staged.record_count ==
                std::numeric_limits<std::uint64_t>::max()) {
                return Status{
                    StatusCode::DataTypeOverflow,
                    "meta record count overflowed u64"
                };
            }
            ++staged.record_count;

            if (record.type == Type::Tombstone) {
                if (staged.tombstone_count ==
                    std::numeric_limits<std::uint64_t>::max()) {
                    return Status{
                        StatusCode::DataTypeOverflow,
                        "meta tombstone count overflowed u64"
                    };
                }
                ++staged.tombstone_count;
            }

            if (!have_record) {
                staged.min_seq_num = record.seq_num;
                staged.max_seq_num = record.seq_num;
                staged.min_key_ptr = record.key_ptr;
                staged.min_key_size = record.key_size;
                staged.max_key_ptr = record.key_ptr;
                staged.max_key_size = record.key_size;
                have_record = true;
                continue;
            }

            staged.min_seq_num = std::min(staged.min_seq_num, record.seq_num);
            staged.max_seq_num = std::max(staged.max_seq_num, record.seq_num);

            if (compare_bytes(
                record.key_ptr,
                record.key_size,
                staged.min_key_ptr,
                staged.min_key_size
            ) < 0) {
                staged.min_key_ptr = record.key_ptr;
                staged.min_key_size = record.key_size;
            }

            if (compare_bytes(
                record.key_ptr,
                record.key_size,
                staged.max_key_ptr,
                staged.max_key_size
            ) > 0) {
                staged.max_key_ptr = record.key_ptr;
                staged.max_key_size = record.key_size;
            }
        }
    }

    if (!have_record) {
        // This is valid only for a genuinely empty data/index section.
        staged = Payload{};
    }

    Status status = staged.validate();
    if (!status.is_ok()) {
        return status;
    }

    status = validate_index_consistency(staged, index_section);
    if (!status.is_ok()) {
        return status;
    }

    Result<Header> staged_header = build_header(staged);
    if (!staged_header.is_ok()) {
        return std::move(staged_header.status);
    }

    payload = staged;
    header = std::move(staged_header.value);
    return Status::ok();
}

Status MetaSection::validate(
    const IndexSection& index_section
) const
{
    Status status = header.validate();
    if (!status.is_ok()) {
        return status;
    }

    status = payload.validate();
    if (!status.is_ok()) {
        return status;
    }

    if (header.payload_size != payload.disk_size()) {
        return Status{
            StatusCode::InvalidPayloadSize,
            "meta header payload size does not match encoded payload"
        };
    }

    std::uint32_t calculated_crc = 0;
    payload.calculate_crc32(calculated_crc);
    if (calculated_crc != header.crc32) {
        return Status{
            StatusCode::ChecksumMismatch,
            "meta payload CRC does not match its header"
        };
    }

    return validate_index_consistency(payload, index_section);
}

Status MetaSection::write(
    WritableFile& file,
    std::uint64_t& offset,
    std::uint64_t& meta_offset
)
{
    Result<Header> staged_header = build_header(payload);
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
            "meta section tracked offset does not match file cursor"
        };
    }

    Status status = align_to_block_boundary(file, offset, BLOCK_SIZE);
    if (!status.is_ok()) {
        return status;
    }

    const std::uint64_t staged_meta_offset = offset;

    status = staged_header.value.write(file, offset);
    if (!status.is_ok()) {
        return status;
    }

    status = payload.write(file, offset);
    if (!status.is_ok()) {
        return status;
    }

    header = std::move(staged_header.value);
    meta_offset = staged_meta_offset;
    return Status::ok();
}

Result<MetaSection> MetaSection::load(
    ReadableFile& file,
    std::uint64_t& offset,
    const IndexSection& index_section,
    Arena& arena,
    std::uint64_t meta_offset
)
{
    const std::uint64_t initial_offset = offset;
    const Arena::Checkpoint checkpoint = arena.checkpoint();

    const auto fail = [&](Status status) -> Result<MetaSection> {
        offset = initial_offset;
        arena.rollback(checkpoint);
        return Result<MetaSection>::fail(std::move(status));
        };

    if (meta_offset % BLOCK_SIZE != 0) {
        return fail(Status{
            StatusCode::InvalidBlockAlignment,
            "meta section offset is not block aligned"
            });
    }

    offset = meta_offset;

    Result<Header> loaded_header = Header::load(file, offset);
    if (!loaded_header.is_ok()) {
        return fail(std::move(loaded_header.status));
    }

    Result<Payload> loaded_payload = Payload::load(
        file,
        offset,
        arena,
        loaded_header.value
    );
    if (!loaded_payload.is_ok()) {
        return fail(std::move(loaded_payload.status));
    }

    MetaSection result{};
    result.header = std::move(loaded_header.value);
    result.payload = std::move(loaded_payload.value);

    Status status = result.validate(index_section);
    if (!status.is_ok()) {
        return fail(std::move(status));
    }

    return Result<MetaSection>::ok(std::move(result));
}

void MetaSection::Payload::calculate_crc32(
    std::uint32_t& crc_buffer
) const noexcept
{
    crc_buffer = ::crc32(0L, Z_NULL, 0);
    crc32_add_pod<std::uint64_t>(crc_buffer, record_count);
    crc32_add_pod<std::uint64_t>(crc_buffer, tombstone_count);
    crc32_add_pod<std::uint64_t>(crc_buffer, min_seq_num);
    crc32_add_pod<std::uint64_t>(crc_buffer, max_seq_num);
    crc32_add_pod<std::uint32_t>(crc_buffer, min_key_size);
    crc32_add_pod<std::uint32_t>(crc_buffer, max_key_size);
    crc32_add_pod<std::uint64_t>(crc_buffer, data_block_count);
    crc32_add_pod<std::uint64_t>(crc_buffer, data_bytes);

    if (min_key_size > 0 && min_key_ptr != nullptr) {
        compute_crc32(crc_buffer, min_key_ptr, min_key_size);
    }
    if (max_key_size > 0 && max_key_ptr != nullptr) {
        compute_crc32(crc_buffer, max_key_ptr, max_key_size);
    }
}

void MetaSection::calculate_crc32(
    std::uint32_t& crc_buffer
) const noexcept
{
    payload.calculate_crc32(crc_buffer);
}