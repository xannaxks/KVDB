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
#include <limits>

using namespace SSTableEntities;


SSTable::SSTable( std::filesystem::path& path,  std::filesystem::path& final_path)
    : path(std::move(path)),
    final_path(std::move(final_path)),
    file_header_section(),
    data_section(),
    index_section(),
    bloom_section(),
    meta_section(),
    file_footer_section()
{
}

SSTable::SSTable(std::filesystem::path& path):
	path(std::move(path)),
	final_path(),
	file_header_section(),
	data_section(),
	index_section(),
	bloom_section(),
	meta_section(),
	file_footer_section()
{
}

Status SSTable::write()
{
	Result<std::unique_ptr<WritableFile>> file_out_opt = open_writable_file(this->path);
    if(!file_out_opt.is_ok())
		return std::move(file_out_opt.status);

    file_out = std::move(file_out_opt.value);
    if (!file_out)
        return Status{ StatusCode::OpenFailed, "Failed to open SSTable for writing path=" + path.string() };

    Status write_result;

    std::uint64_t offset = 0;

    write_result = file_header_section.write(*file_out, offset);
    if (!write_result.is_ok())
        return write_result;

    // Data write also fills index_section. Rewriting an SSTable must not
    // append duplicate index entries from an earlier write attempt.
    index_section = IndexSection{};
    std::uint64_t data_offset = 0;
    write_result = data_section.write(*file_out, offset, this->index_section, data_offset);
    if (!write_result.is_ok())
        return write_result;

    // Build bloom/meta after data/index are finalized
    bloom_section.rebuild(data_section);
    meta_section.rebuild(data_section, index_section);

    std::uint64_t index_offset = 0;
    std::uint64_t bloom_offset = 0;
    std::uint64_t meta_offset = 0;

    write_result = index_section.write(*file_out, offset, index_offset);
    if (!write_result.is_ok())
        return write_result;

    write_result = bloom_section.write(*file_out, offset, bloom_offset);
    if (!write_result.is_ok())
        return write_result;

    write_result = meta_section.write(*file_out, offset, meta_offset);
    if (!write_result.is_ok())
        return write_result;

    file_footer_section.data_offset = data_offset;
    file_footer_section.data_block_count = data_section.data_blocks.size();

    file_footer_section.index_offset = index_offset;
    file_footer_section.index_size = static_cast<std::uint32_t>(index_section.disk_size());

    file_footer_section.bloom_offset = bloom_offset;
    file_footer_section.bloom_size = static_cast<std::uint32_t>(bloom_section.disk_size());

    file_footer_section.meta_offset = meta_offset;
    file_footer_section.meta_size = static_cast<std::uint32_t>(meta_section.disk_size());

    //file_footer_section.finalize(*file_out, offset);
    //file_footer_section.write(*file_out, offset);

	Status align_to_block_result = align_to_block_boundary(*file_out, offset, BLOCK_SIZE);

    if (!align_to_block_result.is_ok())
        return std::move(align_to_block_result);

    file_footer_section.finalize(*file_out, offset);

	write_result = file_footer_section.write(*file_out, offset);
    if (!write_result.is_ok())
        return write_result;

    Status fsync_result = this->fsync(*file_out);
    if (!fsync_result.is_ok())
        return fsync_result;

    file_out = nullptr;

    return Status::ok();
}

Result<SSTable> SSTable::load( std::filesystem::path& path, Arena& arena)
{
    SSTable result(path);
    const Arena::Checkpoint checkpoint = arena.checkpoint();
    std::uint64_t offset = 0ull;

	Result<std::unique_ptr<ReadableFile>> readable_file_opt = open_readable_file(path);
	if (!readable_file_opt.is_ok())
		return Result<SSTable>::fail(std::move(readable_file_opt.status));

    std::unique_ptr<ReadableFile> file = std::move(readable_file_opt.value);

    if (!file)
        return Result<SSTable>::fail(
            Status{
                StatusCode::OpenFailed,
                "Failed to open SSTable for reading path=" + path.string()
            }
        );

    auto file_header_opt = FileHeaderSection::load(*file, offset);
    if (!file_header_opt.is_ok()) return Result<SSTable>::fail(std::move(file_header_opt.status));
    auto file_header_section = std::move(file_header_opt.value);

    auto file_footer_opt = FileFooterSection::load(*file, offset, FileFooterSection::disk_size());
    if (!file_footer_opt.is_ok()) return Result<SSTable>::fail(std::move(file_footer_opt.status));
    auto file_footer_section = std::move(file_footer_opt.value);

    auto data_opt = DataSectionView::load(*file, offset, file_footer_section.data_offset, file_footer_section.data_block_count);
    if (!data_opt.is_ok()) return Result<SSTable>::fail(std::move(data_opt.status));
    auto data_section_view = std::move(data_opt.value);

    auto index_opt = IndexSection::load(*file, offset, arena, file_footer_section.index_offset);
    if (!index_opt.is_ok()) arena.rollback(checkpoint); return Result<SSTable>::fail(std::move(index_opt.status));
    auto index_section = std::move(index_opt.value);

    auto bloom_opt = BloomSection::load(*file, offset, file_footer_section.bloom_offset);
    if (!bloom_opt.is_ok()) { arena.rollback(checkpoint); return Result<SSTable>::fail(std::move(bloom_opt.status)); }
    auto bloom_section = std::move(bloom_opt.value);

    auto meta_opt = MetaSection::load(*file, offset, index_section, arena, file_footer_section.meta_offset);
    if (!meta_opt.is_ok()) arena.rollback(checkpoint); return Result<SSTable>::fail(std::move(meta_opt.status));
    auto meta_section = std::move(meta_opt.value);

    result.file_header_section = std::move(file_header_section);
    result.data_section_view = std::move(data_section_view);
    result.index_section = std::move(index_section);
    result.bloom_section = std::move(bloom_section);
    result.meta_section = std::move(meta_section);
    result.file_footer_section = std::move(file_footer_section);
    result.file_in = std::move(file);

    return Result<SSTable>::ok(std::move(result));
}

Status SSTable::fsync(WritableFile& file_out)
{
	Status results = file_out.sync();

    if (!results.is_ok())
        return results;

    results = file_out.close();

    if (!results.is_ok())
        return results;

    results = file_out.durable_rename(this->final_path, true);

    if (!results.is_ok())
        return results;

    results = file_out.sync_parent_directory();

    if (!results.is_ok())
        return results;

    return Status::ok();
}

const SSTableEntities::FileHeaderSection& SSTable::get_file_header_section() const
{
    return this->file_header_section;
}
const SSTableEntities::DataSection& SSTable::get_data_section() const
{
    return this->data_section;
}
const SSTableEntities::DataSectionView& SSTable::get_data_section_view() const
{
    return this->data_section_view;
}
const SSTableEntities::IndexSection& SSTable::get_index_section() const
{
    return this->index_section;
}
const SSTableEntities::BloomSection& SSTable::get_bloom_section() const
{
    return this->bloom_section;
}
const SSTableEntities::MetaSection& SSTable::get_meta_section() const
{
    return this->meta_section;
}
const SSTableEntities::FileFooterSection& SSTable::get_file_footer_section() const
{
    return this->file_footer_section;
}

const std::filesystem::path& SSTable::get_path() const
{
    return this->path;
}
const std::filesystem::path& SSTable::get_final_path() const
{
    return this->final_path;
} 

Status SSTable::append_record(const InternalRecord& record)
{
	return this->data_section.add_payload(record);
}

std::size_t SSTable::fixed_disk_size()
{
    return FileHeaderSection::disk_size() +
        MetaSection::fixed_disk_size() +
        BloomSection::disk_size() + 
		FileFooterSection::disk_size();
}