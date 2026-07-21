#include "sstable_entities/file_footer_section.h"

#include <format>
#include <limits>
#include <utility>

#include "crc32_helpers.h"
#include "endian_io.h"
#include "file_helpers.h"
#include "sstable_entities/bloom_section.h"
#include "sstable_entities/data_section.h"
#include "sstable_entities/index_section.h"
#include "sstable_entities/meta_section.h"

using namespace SSTableEntities;

namespace
{
    constexpr std::size_t kSectionHeaderDiskSize =
        sizeof(std::uint8_t) +
        sizeof(std::uint32_t) +
        sizeof(std::uint32_t);

    constexpr std::size_t kBloomPayloadDiskSizeV1 =
        sizeof(std::uint64_t) +
        sizeof(std::uint32_t) +
        sizeof(std::uint32_t) +
        BLOOM_MASK_BIT_SIZE;

    constexpr std::size_t kBloomSectionDiskSizeV1 =
        kSectionHeaderDiskSize + kBloomPayloadDiskSizeV1;

    constexpr std::size_t kMetaFixedPayloadDiskSizeV1 =
        6u * sizeof(std::uint64_t) +
        2u * sizeof(std::uint32_t);

    constexpr std::size_t kMetaMinimumDiskSizeV1 =
        kSectionHeaderDiskSize + kMetaFixedPayloadDiskSizeV1;

    [[nodiscard]] bool is_block_aligned(std::uint64_t value) noexcept
    {
        return value % BLOCK_SIZE == 0;
    }

    [[nodiscard]] Status validate_section_span(
        std::uint64_t section_offset,
        std::uint32_t logical_size,
        std::uint64_t next_section_offset,
        const char* section_name
    )
    {
        if (section_offset >= next_section_offset) {
            return Status{
                StatusCode::OffsetOverlap,
                std::format(
                    "{} section does not begin before the following section",
                    section_name
                )
            };
        }

        const std::uint64_t physical_span =
            next_section_offset - section_offset;

        if (static_cast<std::uint64_t>(logical_size) > physical_span) {
            return Status{
                StatusCode::OffsetOverlap,
                std::format(
                    "{} logical size exceeds the physical span before the following section",
                    section_name
                )
            };
        }

        return Status::ok();
    }

    [[nodiscard]] Status checked_u32_size(
        std::size_t value,
        const char* field_name,
        std::uint32_t& output
    )
    {
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            return Status{
                StatusCode::DataTypeOverflow,
                std::format(
                    "{} cannot be represented by the footer's u32 size field",
                    field_name
                )
            };
        }

        output = static_cast<std::uint32_t>(value);
        return Status::ok();
    }
}

FileFooterSection::FileFooterSection() noexcept
{
    calculate_crc32(footer_crc32);
}

Status FileFooterSection::validate(
    std::uint64_t footer_offset,
    std::uint64_t actual_file_size
) const
{
    if (magic != FILE_FOOTER_MAGIC) {
        return Status{
            StatusCode::BadMagic,
            std::format(
                "invalid SSTable footer magic: expected=0x{:08x}, actual=0x{:08x}",
                FILE_FOOTER_MAGIC,
                magic
            )
        };
    }

    if (version == 0 || version > LATEST_SSTABLE_VERSION) {
        return Status{
            StatusCode::UnsupportedVersion,
            std::format(
                "unsupported SSTable footer version: file_version={}, latest_supported={}",
                version,
                LATEST_SSTABLE_VERSION
            )
        };
    }

    if (reserved != 0) {
        return Status{
            StatusCode::InvalidFooter,
            "SSTable footer reserved field must be zero"
        };
    }

    if (!is_block_aligned(footer_offset)) {
        return Status{
            StatusCode::InvalidBlockAlignment,
            "SSTable footer must begin at a physical block boundary"
        };
    }

    if (footer_offset >
        std::numeric_limits<std::uint64_t>::max() - disk_size()) {
        return Status{
            StatusCode::DataTypeOverflow,
            "footer offset plus footer size overflows uint64_t"
        };
    }

    const std::uint64_t expected_file_size =
        footer_offset + static_cast<std::uint64_t>(disk_size());

    if (actual_file_size != expected_file_size ||
        file_size != actual_file_size) {
        return Status{
            StatusCode::InvalidFooter,
            std::format(
                "footer is not the final object in the file or records the wrong file size: footer_offset={}, encoded_file_size={}, actual_file_size={}",
                footer_offset,
                file_size,
                actual_file_size
            )
        };
    }

    if (!is_block_aligned(index_offset) ||
        !is_block_aligned(bloom_offset) ||
        !is_block_aligned(meta_offset)) {
        return Status{
            StatusCode::InvalidSectionOffset,
            "index, Bloom, and metadata section offsets must be block aligned"
        };
    }

    if (data_block_count > 0) {
        if (!is_block_aligned(data_offset)) {
            return Status{
                StatusCode::InvalidSectionOffset,
                "a nonempty data section must begin at a block boundary"
            };
        }

        if (data_block_count >
            (std::numeric_limits<std::uint64_t>::max() - data_offset) /
            BLOCK_SIZE) {
            return Status{
                StatusCode::DataTypeOverflow,
                "data section physical span overflows uint64_t"
            };
        }

        const std::uint64_t expected_index_offset =
            data_offset + data_block_count * BLOCK_SIZE;

        if (expected_index_offset != index_offset) {
            return Status{
                StatusCode::OffsetOverlap,
                std::format(
                    "index offset does not immediately follow the declared data blocks: expected={}, actual={}",
                    expected_index_offset,
                    index_offset
                )
            };
        }
    }
    else if (data_offset > index_offset) {
        return Status{
            StatusCode::InvalidSectionOffset,
            "empty data-section cursor cannot be after the index section"
        };
    }

    if (index_size < kSectionHeaderDiskSize) {
        return Status{
            StatusCode::InvalidSectionSize,
            "index section is smaller than its encoded header"
        };
    }

    if (bloom_size != kBloomSectionDiskSizeV1) {
        return Status{
            StatusCode::InvalidSectionSize,
            std::format(
                "version-1 Bloom section size must be {}, actual={}",
                kBloomSectionDiskSizeV1,
                bloom_size
            )
        };
    }

    if (meta_size < kMetaMinimumDiskSizeV1 || meta_size > BLOCK_SIZE) {
        return Status{
            StatusCode::InvalidSectionSize,
            std::format(
                "metadata section size must be in [{}, {}], actual={}",
                kMetaMinimumDiskSizeV1,
                BLOCK_SIZE,
                meta_size
            )
        };
    }

    Status span_status = validate_section_span(
        index_offset,
        index_size,
        bloom_offset,
        "index"
    );
    if (!span_status.is_ok()) {
        return span_status;
    }

    span_status = validate_section_span(
        bloom_offset,
        bloom_size,
        meta_offset,
        "Bloom"
    );
    if (!span_status.is_ok()) {
        return span_status;
    }

    span_status = validate_section_span(
        meta_offset,
        meta_size,
        footer_offset,
        "metadata"
    );
    if (!span_status.is_ok()) {
        return span_status;
    }

    std::uint32_t expected_crc = 0;
    calculate_crc32(expected_crc);
    if (expected_crc != footer_crc32) {
        return Status{
            StatusCode::ChecksumMismatch,
            std::format(
                "SSTable footer CRC mismatch: expected={}, actual={}",
                expected_crc,
                footer_crc32
            )
        };
    }

    return Status::ok();
}

Status FileFooterSection::finalize(
    WritableFile& file,
    std::uint64_t footer_offset
)
{
    Result<std::uint64_t> position_result = file.current_position();
    if (!position_result.is_ok()) {
        return std::move(position_result.status);
    }

    if (position_result.value != footer_offset) {
        return Status{
            StatusCode::InvalidOffset,
            "tracked footer offset does not match the writable file cursor"
        };
    }

    if (!is_block_aligned(footer_offset)) {
        return Status{
            StatusCode::InvalidBlockAlignment,
            "footer finalization requires a block-aligned cursor"
        };
    }

    std::uint64_t current_file_size = 0;
    Status status = file.get_file_size(current_file_size);
    if (!status.is_ok()) {
        return status;
    }

    if (current_file_size != footer_offset) {
        return Status{
            StatusCode::InvalidState,
            "footer must be appended exactly at the current end of file"
        };
    }

    if (footer_offset >
        std::numeric_limits<std::uint64_t>::max() - disk_size()) {
        return Status{
            StatusCode::DataTypeOverflow,
            "footer offset plus footer size overflows uint64_t"
        };
    }

    FileFooterSection staged = *this;
    staged.magic = FILE_FOOTER_MAGIC;
    staged.version = LATEST_SSTABLE_VERSION;
    staged.reserved = 0;
    staged.file_size =
        footer_offset + static_cast<std::uint64_t>(disk_size());
    staged.calculate_crc32(staged.footer_crc32);

    status = staged.validate(footer_offset, staged.file_size);
    if (!status.is_ok()) {
        return status;
    }

    *this = staged;
    return Status::ok();
}

Status FileFooterSection::write(
    WritableFile& file,
    std::uint64_t& offset
)
{
    FileFooterSection staged = *this;
    Status status = staged.finalize(file, offset);
    if (!status.is_ok()) {
        return status;
    }

    status = fits_in_block(offset, disk_size(), BLOCK_SIZE);
    if (!status.is_ok()) {
        return status;
    }

    status = kvdb::blockio::write_u32_t_le(
        file, staged.magic, offset, BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u32_t_le(
        file, staged.version, offset, BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u32_t_le(
        file, staged.reserved, offset, BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u64_t_le(
        file, staged.data_offset, offset, BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u64_t_le(
        file, staged.data_block_count, offset, BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u64_t_le(
        file, staged.index_offset, offset, BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u32_t_le(
        file, staged.index_size, offset, BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u64_t_le(
        file, staged.bloom_offset, offset, BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u32_t_le(
        file, staged.bloom_size, offset, BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u64_t_le(
        file, staged.meta_offset, offset, BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u32_t_le(
        file, staged.meta_size, offset, BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u64_t_le(
        file, staged.file_size, offset, BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    status = kvdb::blockio::write_u32_t_le(
        file, staged.footer_crc32, offset, BLOCK_SIZE
    );
    if (!status.is_ok()) return status;

    *this = staged;
    return Status::ok();
}

Result<FileFooterSection> FileFooterSection::load(
    ReadableFile& file,
    std::uint64_t& offset,
    std::uint64_t file_footer_backwards_offset
)
{
    const std::uint64_t initial_offset = offset;

    const auto fail = [&](Status status) -> Result<FileFooterSection>
        {
            offset = initial_offset;
            return Result<FileFooterSection>::fail(std::move(status));
        };

    std::uint64_t actual_file_size = 0;
    Status status = file.get_file_size(actual_file_size);
    if (!status.is_ok()) {
        return fail(std::move(status));
    }

    if (actual_file_size < disk_size()) {
        return fail(Status{
            StatusCode::UnexpectedEOF,
            "file is too small to contain an SSTable footer"
            });
    }

    std::uint64_t footer_offset = offset;
    if (file_footer_backwards_offset != 0) {
        if (file_footer_backwards_offset > actual_file_size) {
            return fail(Status{
                StatusCode::OffsetOutOfRange,
                "footer backwards offset exceeds the file size"
                });
        }

        footer_offset = actual_file_size - file_footer_backwards_offset;
    }

    if (!is_block_aligned(footer_offset)) {
        return fail(Status{
            StatusCode::InvalidBlockAlignment,
            "SSTable footer offset is not block aligned"
            });
    }

    status = can_read_range(
        footer_offset,
        disk_size(),
        actual_file_size
    );
    if (!status.is_ok()) {
        return fail(Status{
            StatusCode::UnexpectedEOF,
            "complete SSTable footer is not present at the requested offset"
            });
    }

    if (footer_offset + disk_size() != actual_file_size) {
        return fail(Status{
            StatusCode::InvalidFooter,
            "SSTable footer must be the final encoded object in the file"
            });
    }

    status = fits_in_block(footer_offset, disk_size(), BLOCK_SIZE);
    if (!status.is_ok()) {
        return fail(status);
    }

    std::uint64_t cursor = footer_offset;
    FileFooterSection result{};

    status = kvdb::blockio::read_u32_t_le(
        file, result.magic, cursor, BLOCK_SIZE
    );
    if (!status.is_ok()) return fail(std::move(status));

    status = kvdb::blockio::read_u32_t_le(
        file, result.version, cursor, BLOCK_SIZE
    );
    if (!status.is_ok()) return fail(std::move(status));

    status = kvdb::blockio::read_u32_t_le(
        file, result.reserved, cursor, BLOCK_SIZE
    );
    if (!status.is_ok()) return fail(std::move(status));

    status = kvdb::blockio::read_u64_t_le(
        file, result.data_offset, cursor, BLOCK_SIZE
    );
    if (!status.is_ok()) return fail(std::move(status));

    status = kvdb::blockio::read_u64_t_le(
        file, result.data_block_count, cursor, BLOCK_SIZE
    );
    if (!status.is_ok()) return fail(std::move(status));

    status = kvdb::blockio::read_u64_t_le(
        file, result.index_offset, cursor, BLOCK_SIZE
    );
    if (!status.is_ok()) return fail(std::move(status));

    status = kvdb::blockio::read_u32_t_le(
        file, result.index_size, cursor, BLOCK_SIZE
    );
    if (!status.is_ok()) return fail(std::move(status));

    status = kvdb::blockio::read_u64_t_le(
        file, result.bloom_offset, cursor, BLOCK_SIZE
    );
    if (!status.is_ok()) return fail(std::move(status));

    status = kvdb::blockio::read_u32_t_le(
        file, result.bloom_size, cursor, BLOCK_SIZE
    );
    if (!status.is_ok()) return fail(std::move(status));

    status = kvdb::blockio::read_u64_t_le(
        file, result.meta_offset, cursor, BLOCK_SIZE
    );
    if (!status.is_ok()) return fail(std::move(status));

    status = kvdb::blockio::read_u32_t_le(
        file, result.meta_size, cursor, BLOCK_SIZE
    );
    if (!status.is_ok()) return fail(std::move(status));

    status = kvdb::blockio::read_u64_t_le(
        file, result.file_size, cursor, BLOCK_SIZE
    );
    if (!status.is_ok()) return fail(std::move(status));

    status = kvdb::blockio::read_u32_t_le(
        file, result.footer_crc32, cursor, BLOCK_SIZE
    );
    if (!status.is_ok()) return fail(std::move(status));

    std::uint32_t expected_crc = 0;
    result.calculate_crc32(expected_crc);
    if (expected_crc != result.footer_crc32) {
        return fail(Status{
            StatusCode::ChecksumMismatch,
            std::format(
                "SSTable footer CRC mismatch: expected={}, actual={}",
                expected_crc,
                result.footer_crc32
            )
            });
    }

    status = result.validate(footer_offset, actual_file_size);
    if (!status.is_ok()) {
        return fail(status);
    }

    offset = cursor;
    return Result<FileFooterSection>::ok(std::move(result));
}

Status FileFooterSection::rebuild(
    const DataSection& data_section,
    std::uint64_t data_section_offset
)
{
    if (!data_section.data_blocks.empty() &&
        !is_block_aligned(data_section_offset)) {
        return Status{
            StatusCode::InvalidSectionOffset,
            "nonempty data section offset must be block aligned"
        };
    }

    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (data_section.data_blocks.size() >
            std::numeric_limits<std::uint64_t>::max()) {
            return Status{
                StatusCode::DataTypeOverflow,
                "data block count cannot be represented by the footer"
            };
        }
    }

    data_offset = data_section_offset;
    data_block_count =
        static_cast<std::uint64_t>(data_section.data_blocks.size());
    return Status::ok();
}

Status FileFooterSection::rebuild(
    IndexSection& index_section,
    std::uint64_t index_section_offset
)
{
    if (!is_block_aligned(index_section_offset)) {
        return Status{
            StatusCode::InvalidSectionOffset,
            "index section offset must be block aligned"
        };
    }

    std::uint32_t encoded_size = 0;
    Status status = checked_u32_size(
        index_section.disk_size(),
        "index section size",
        encoded_size
    );
    if (!status.is_ok()) {
        return status;
    }

    if (encoded_size < kSectionHeaderDiskSize) {
        return Status{
            StatusCode::InvalidSectionSize,
            "index section is smaller than its encoded header"
        };
    }

    index_offset = index_section_offset;
    index_size = encoded_size;
    return Status::ok();
}

Status FileFooterSection::rebuild(
    BloomSection& bloom_section,
    std::uint64_t bloom_section_offset
)
{
    if (!is_block_aligned(bloom_section_offset)) {
        return Status{
            StatusCode::InvalidSectionOffset,
            "Bloom section offset must be block aligned"
        };
    }

    std::uint32_t encoded_size = 0;
    Status status = checked_u32_size(
        bloom_section.disk_size(),
        "Bloom section size",
        encoded_size
    );
    if (!status.is_ok()) {
        return status;
    }

    bloom_offset = bloom_section_offset;
    bloom_size = encoded_size;
    return Status::ok();
}

Status FileFooterSection::rebuild(
    MetaSection& meta_section,
    std::uint64_t meta_section_offset
)
{
    if (!is_block_aligned(meta_section_offset)) {
        return Status{
            StatusCode::InvalidSectionOffset,
            "metadata section offset must be block aligned"
        };
    }

    std::uint32_t encoded_size = 0;
    Status status = checked_u32_size(
        meta_section.disk_size(),
        "metadata section size",
        encoded_size
    );
    if (!status.is_ok()) {
        return status;
    }

    meta_offset = meta_section_offset;
    meta_size = encoded_size;
    return Status::ok();
}

void FileFooterSection::calculate_crc32(
    std::uint32_t& crc_buffer
) const noexcept
{
    crc_buffer = ::crc32(0L, Z_NULL, 0);
    crc32_add_pod<std::uint32_t>(crc_buffer, magic);
    crc32_add_pod<std::uint32_t>(crc_buffer, version);
    crc32_add_pod<std::uint32_t>(crc_buffer, reserved);
    crc32_add_pod<std::uint64_t>(crc_buffer, data_offset);
    crc32_add_pod<std::uint64_t>(crc_buffer, data_block_count);
    crc32_add_pod<std::uint64_t>(crc_buffer, index_offset);
    crc32_add_pod<std::uint32_t>(crc_buffer, index_size);
    crc32_add_pod<std::uint64_t>(crc_buffer, bloom_offset);
    crc32_add_pod<std::uint32_t>(crc_buffer, bloom_size);
    crc32_add_pod<std::uint64_t>(crc_buffer, meta_offset);
    crc32_add_pod<std::uint32_t>(crc_buffer, meta_size);
    crc32_add_pod<std::uint64_t>(crc_buffer, file_size);
}