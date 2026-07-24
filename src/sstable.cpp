#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#endif

#include "sstable.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

using namespace SSTableEntities;

namespace
{
    [[nodiscard]] bool range_fits(
        std::uint64_t offset,
        std::uint64_t size,
        std::uint64_t limit
    ) noexcept
    {
        return offset <= limit && size <= limit - offset;
    }

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

    [[nodiscard]] bool fits_u32(std::uint64_t value) noexcept
    {
        return value <= std::numeric_limits<std::uint32_t>::max();
    }
}

Status SSTable::write()
{
    /*
     * Only a newly constructed builder SSTable may be written.
     *
     * Loaded SSTables are immutable.
     * Published SSTables must not be rewritten because path now points to
     * the final file and opening it for writing could truncate it.
     */
    if (state != State::Building)
    {
        return Status{
            StatusCode::InvalidState,
            "Cannot write a loaded, published, or uninitialized SSTable"
        };
    }

    if (path.empty())
    {
        return Status{
            StatusCode::InvalidState,
            "Cannot write SSTable: temporary path is empty"
        };
    }

    if (final_path.empty())
    {
        return Status{
            StatusCode::InvalidState,
            "Cannot write SSTable: final path is empty"
        };
    }

    /*
     * Never write directly to the destination file. The temporary file must
     * be separate so that an incomplete write cannot destroy the existing
     * final SSTable.
     */
    if (path.lexically_normal() == final_path.lexically_normal())
    {
        return Status{
            StatusCode::InvalidArgument,
            "SSTable temporary path and final path must be different"
        };
    }

    auto file_out_result = open_writable_file(path);

    if (!file_out_result.is_ok())
        return std::move(file_out_result.status);

    std::unique_ptr<WritableFile> atomic_file_out =
        std::move(file_out_result.value);

    if (!atomic_file_out)
    {
        return Status{
            StatusCode::OpenFailed,
            "Failed to open SSTable for writing path=" + path.string()
        };
    }

    std::uint64_t offset = 0;

    Status write_result =
        file_header_section.write(*atomic_file_out, offset);

    if (!write_result.is_ok())
        return write_result;

    /*
     * DataSection::write() rebuilds the index while writing the data.
     * Clear an index left by a previous failed write attempt.
     */
    index_section = IndexSection{};

    std::uint64_t data_offset = 0;

    write_result = data_section.write(
        *atomic_file_out,
        offset,
        index_section,
        data_offset
    );

    if (!write_result.is_ok())
        return write_result;

    /*
     * These sections depend on the finalized data and index contents.
     */
    bloom_section.rebuild(data_section);
    meta_section.rebuild(data_section, index_section);

    std::uint64_t index_offset = 0;
    std::uint64_t bloom_offset = 0;
    std::uint64_t meta_offset = 0;

    write_result = index_section.write(
        *atomic_file_out,
        offset,
        index_offset
    );

    if (!write_result.is_ok())
        return write_result;

    write_result = bloom_section.write(
        *atomic_file_out,
        offset,
        bloom_offset
    );

    if (!write_result.is_ok())
        return write_result;

    write_result = meta_section.write(
        *atomic_file_out,
        offset,
        meta_offset
    );

    if (!write_result.is_ok())
        return write_result;

    const std::uint64_t index_size = index_section.disk_size();
    const std::uint64_t bloom_size = bloom_section.disk_size();
    const std::uint64_t meta_size = meta_section.disk_size();

    if (!fits_u32(index_size) ||
        !fits_u32(bloom_size) ||
        !fits_u32(meta_size))
    {
        return Status{
            StatusCode::InvalidSectionSize,
            "SSTable section size cannot be represented in the footer"
        };
    }

    file_footer_section.data_offset = data_offset;
    file_footer_section.data_block_count =
        data_section.data_blocks.size();

    file_footer_section.index_offset = index_offset;
    file_footer_section.index_size =
        static_cast<std::uint32_t>(index_size);

    file_footer_section.bloom_offset = bloom_offset;
    file_footer_section.bloom_size =
        static_cast<std::uint32_t>(bloom_size);

    file_footer_section.meta_offset = meta_offset;
    file_footer_section.meta_size =
        static_cast<std::uint32_t>(meta_size);

    Status alignment_result = align_to_block_boundary(
        *atomic_file_out,
        offset,
        BLOCK_SIZE
    );

    if (!alignment_result.is_ok())
        return alignment_result;

    Status finalize_result =
        file_footer_section.finalize(*atomic_file_out, offset);

    if (!finalize_result.is_ok())
        return finalize_result;

    write_result =
        file_footer_section.write(*atomic_file_out, offset);

    if (!write_result.is_ok())
        return write_result;

    Status publish_result = fsync(*atomic_file_out);

    if (!publish_result.is_ok())
        return publish_result;

    return Status::ok();
}

Result<SSTable> SSTable::load(
    const std::filesystem::path& path,
    Arena& arena
)
{
    if (path.empty())
    {
        return Result<SSTable>::fail(Status{
            StatusCode::InvalidArgument,
            "Cannot load an SSTable from an empty path"
            });
    }

    /*
     * The one-path constructor marks the table as Loaded, so append_record()
     * and write() will reject it.
     */
    SSTable result(path);

    const Arena::Checkpoint checkpoint = arena.checkpoint();

    auto fail = [&](Status status) -> Result<SSTable>
        {
            arena.rollback(checkpoint);
            return Result<SSTable>::fail(std::move(status));
        };

    auto file_result = open_readable_file(path);

    if (!file_result.is_ok())
        return fail(std::move(file_result.status));

    std::unique_ptr<ReadableFile> file =
        std::move(file_result.value);

    if (!file)
    {
        return fail(Status{
            StatusCode::OpenFailed,
            "Failed to open SSTable for reading path=" + path.string()
            });
    }

    std::uint64_t header_cursor = 0;

    auto header_result =
        FileHeaderSection::load(*file, header_cursor);

    if (!header_result.is_ok())
        return fail(std::move(header_result.status));

    /*
     * This assumes FileFooterSection::load() leaves footer_cursor containing
     * the footer's starting offset, as required by your current validation.
     */
    std::uint64_t footer_cursor = 0;

    auto footer_result = FileFooterSection::load(
        *file,
        footer_cursor,
        FileFooterSection::disk_size()
    );

    if (!footer_result.is_ok())
        return fail(std::move(footer_result.status));

    FileFooterSection footer =
        std::move(footer_result.value);

    /*
     * Validate the declared physical order of all sections before trusting
     * their offsets.
     */
    if (header_cursor > footer.data_offset)
    {
        return fail(Status{
            StatusCode::InvalidState,
            "SSTable file header overlaps with or comes after data section"
            });
    }

    /*
     * Prevent an invalid block count from overflowing during multiplication.
     */
    constexpr std::uint64_t maximum_u64 =
        std::numeric_limits<std::uint64_t>::max();

    const std::uint64_t block_size =
        static_cast<std::uint64_t>(BLOCK_SIZE);

    const std::uint64_t data_block_count =
        static_cast<std::uint64_t>(footer.data_block_count);

    if (block_size == 0 ||
        data_block_count > maximum_u64 / block_size)
    {
        return fail(Status{
            StatusCode::InvalidState,
            "SSTable data block count overflows its data section size"
            });
    }

    const std::uint64_t data_section_size =
        data_block_count * block_size;

    if (!range_fits(
        footer.data_offset,
        data_section_size,
        footer.index_offset))
    {
        return fail(Status{
            StatusCode::InvalidState,
            "SSTable data section overlaps with or comes after index section"
            });
    }

    if (!range_fits(
        footer.index_offset,
        footer.index_size,
        footer.bloom_offset))
    {
        return fail(Status{
            StatusCode::InvalidState,
            "SSTable index section overlaps with or comes after bloom section"
            });
    }

    if (!range_fits(
        footer.bloom_offset,
        footer.bloom_size,
        footer.meta_offset))
    {
        return fail(Status{
            StatusCode::InvalidState,
            "SSTable bloom section overlaps with or comes after meta section"
            });
    }

    /*
     * Use meta_size too. Checking only meta_offset would allow metadata bytes
     * to overlap the footer.
     */
    if (!range_fits(
        footer.meta_offset,
        footer.meta_size,
        footer_cursor))
    {
        return fail(Status{
            StatusCode::InvalidState,
            "SSTable meta section overlaps with or comes after file footer"
            });
    }

    std::uint64_t data_cursor = footer.data_offset;

    auto data_result = DataSectionView::load(
        *file,
        data_cursor,
        footer.data_offset,
        footer.data_block_count,
        footer.index_offset
    );

    if (!data_result.is_ok())
        return fail(std::move(data_result.status));
    if (data_cursor != footer.index_offset)
    {
        return fail(Status{
            StatusCode::InvalidSectionSize,
            "SSTable data section does not end at the index section"
            });
    }

    std::uint64_t declared_index_end = 0;
    std::uint64_t declared_bloom_end = 0;
    std::uint64_t declared_meta_end = 0;

    if (!checked_add_u64(
        footer.index_offset,
        footer.index_size,
        declared_index_end) ||
        !checked_add_u64(
            footer.bloom_offset,
            footer.bloom_size,
            declared_bloom_end) ||
        !checked_add_u64(
            footer.meta_offset,
            footer.meta_size,
            declared_meta_end))
    {
        return fail(Status{
            StatusCode::InvalidSectionSize,
            "SSTable declared section end overflows uint64_t"
            });
    }

    std::uint64_t index_cursor = footer.index_offset;

    auto index_result = IndexSection::load(
        *file,
        index_cursor,
        arena,
        footer.index_offset
    );

    if (!index_result.is_ok())
        return fail(std::move(index_result.status));

    if (index_cursor != declared_index_end)
    {
        return fail(Status{
            StatusCode::InvalidSectionSize,
            "SSTable index loader consumed a different size than declared"
            });
    }

    IndexSection index =
        std::move(index_result.value);

    std::uint64_t bloom_cursor = footer.bloom_offset;

    auto bloom_result = BloomSection::load(
        *file,
        bloom_cursor,
        footer.bloom_offset
    );

    if (!bloom_result.is_ok())
        return fail(std::move(bloom_result.status));

    if (bloom_cursor != declared_bloom_end)
    {
        return fail(Status{
            StatusCode::InvalidSectionSize,
            "SSTable bloom loader consumed a different size than declared"
            });
    }

    std::uint64_t meta_cursor = footer.meta_offset;

    auto meta_result = MetaSection::load(
        *file,
        meta_cursor,
        index,
        arena,
        footer.meta_offset
    );

    if (!meta_result.is_ok())
        return fail(std::move(meta_result.status));

    if (meta_cursor != declared_meta_end)
    {
        return fail(Status{
            StatusCode::InvalidSectionSize,
            "SSTable metadata loader consumed a different size than declared"
            });
    }

    result.file_header_section =
        std::move(header_result.value);

    result.file_footer_section =
        std::move(footer);

    result.data_section_view =
        std::move(data_result.value);

    result.index_section =
        std::move(index);

    result.bloom_section =
        std::move(bloom_result.value);

    result.meta_section =
        std::move(meta_result.value);

    result.file_in =
        std::move(file);

    return Result<SSTable>::ok(std::move(result));
}

Status SSTable::fsync(WritableFile& file_out)
{
    Status result = file_out.sync();

    if (!result.is_ok())
        return result;

    result = file_out.close();

    if (!result.is_ok())
        return result;

    result = file_out.durable_rename(final_path, true);

    if (!result.is_ok())
        return result;

    /*
     * The rename has already happened. Mark the SSTable published before
     * syncing the parent directory.
     *
     * If directory syncing fails, callers must not retry write(), because
     * doing so could truncate the newly published final file.
     */
    state = State::Published;
    path = final_path;

    result = file_out.sync_parent_directory();

    if (!result.is_ok())
        return result;

    return Status::ok();
}

const SSTableEntities::FileHeaderSection&
SSTable::get_file_header_section() const
{
    return file_header_section;
}

const SSTableEntities::DataSection&
SSTable::get_data_section() const
{
    return data_section;
}

const SSTableEntities::DataSectionView&
SSTable::get_data_section_view() const
{
    return data_section_view;
}

const SSTableEntities::IndexSection&
SSTable::get_index_section() const
{
    return index_section;
}

const SSTableEntities::BloomSection&
SSTable::get_bloom_section() const
{
    return bloom_section;
}

const SSTableEntities::MetaSection&
SSTable::get_meta_section() const
{
    return meta_section;
}

const SSTableEntities::FileFooterSection&
SSTable::get_file_footer_section() const
{
    return file_footer_section;
}

const std::filesystem::path&
SSTable::get_path() const
{
    return path;
}

const std::filesystem::path&
SSTable::get_final_path() const
{
    return final_path;
}

Status SSTable::append_record(
    const InternalRecord& record
)
{
    if (state != State::Building)
    {
        return Status{
            StatusCode::InvalidState,
            "Cannot append a record to a loaded, published, or uninitialized SSTable"
        };
    }

    return data_section.add_payload(record);
}

std::size_t SSTable::fixed_disk_size() noexcept
{
    return FileHeaderSection::disk_size() +
        MetaSection::fixed_disk_size() +
        BloomSection::disk_size() +
        FileFooterSection::disk_size();
}

Result<std::optional<InternalRecord>> SSTable::get(
    const ArenaEntry& key,
    Arena& arena
) const
{
    if (state != State::Loaded || !file_in) {
        return Result<std::optional<InternalRecord>>::fail(Status{
            StatusCode::InvalidState,
            "get() requires an SSTable loaded from an immutable file"
            });
    }

    if (key.size > 0 && key.data == nullptr) {
        return Result<std::optional<InternalRecord>>::fail(Status{
            StatusCode::InvalidArgument,
            "searched key has a non-zero size but a null data pointer"
            });
    }

    Result<std::size_t> block_result =
        index_section.find_first_candidate(
            reinterpret_cast<const void*>(key.data),
            key.size
        );

    if (!block_result.is_ok()) {
        return Result<std::optional<InternalRecord>>::fail(
            std::move(block_result.status)
        );
    }

    const std::size_t block_index = block_result.value;
    if (block_index >= data_section_view.data_blocks.size()) {
        return Result<std::optional<InternalRecord>>::fail(Status{
            StatusCode::Corruption,
            "index points outside the SSTable data section"
            });
    }

    Result<std::optional<std::size_t>> record_index_result =
        data_section_view.find_first_record(
            *file_in,
            block_index,
            key
        );

    if (!record_index_result.is_ok()) {
        return Result<std::optional<InternalRecord>>::fail(
            std::move(record_index_result.status)
        );
    }

    if (!record_index_result.value.has_value()) {
        return Result<std::optional<InternalRecord>>::ok(std::nullopt);
    }

    Result<InternalRecord> record_result =
        data_section_view.read_record(
            *file_in,
            block_index,
            *record_index_result.value,
            arena
        );

    if (!record_result.is_ok()) {
        return Result<std::optional<InternalRecord>>::fail(
            std::move(record_result.status)
        );
    }

    return Result<std::optional<InternalRecord>>::ok(
        std::optional<InternalRecord>{ std::move(record_result.value) }
    );
}