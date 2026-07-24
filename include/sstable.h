#pragma once

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#endif

#include <algorithm>
#include <bitset>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "MurmurHash3.h"
#include "crc32_helpers.h"
#include "endian_io.h"
#include "file.h"
#include "file_helpers.h"
#include "mem_table.h"
#include "record.h"
#include "sstable_entities.h"
#include "sstable_entities/bloom_section.h"
#include "sstable_entities/data_section.h"
#include "sstable_entities/data_section_view.h"
#include "sstable_entities/file_footer_section.h"
#include "sstable_entities/file_header_section.h"
#include "sstable_entities/index_section.h"
#include "sstable_entities/meta_section.h"
#include "status.h"

class SSTable
{
private:
    enum class State : std::uint8_t
    {
        Empty,
        Building,
        Loaded,
        Published
    };

public:
    SSTable() noexcept = default;

    SSTable(
        std::filesystem::path temporary_path,
        std::filesystem::path destination_path
    )
        : path(std::move(temporary_path)),
        final_path(std::move(destination_path)),
        state(State::Building)
    {
    }

    explicit SSTable(std::filesystem::path existing_path)
        : path(existing_path),
        final_path(std::move(existing_path)),
        state(State::Loaded)
    {
    }

    SSTable(const SSTable&) = delete;
    SSTable& operator=(const SSTable&) = delete;

    SSTable(SSTable&&) noexcept = default;
    SSTable& operator=(SSTable&&) noexcept = default;

private:
    std::filesystem::path path;
    std::filesystem::path final_path;

    State state{ State::Empty };

    std::unique_ptr<ReadableFile> file_in;

    SSTableEntities::FileHeaderSection file_header_section{};
    SSTableEntities::DataSection data_section{};
    SSTableEntities::DataSectionView data_section_view{};
    SSTableEntities::IndexSection index_section{};
    SSTableEntities::BloomSection bloom_section{};
    SSTableEntities::MetaSection meta_section{};
    SSTableEntities::FileFooterSection file_footer_section{};

    [[nodiscard]] Status fsync(WritableFile& file_out);

    friend class SSTableManager;
    friend class SSTableWriter;
    friend class SSTableLoader;
    friend class SSTableIterator;

public:
    [[nodiscard]] Status write();

    [[nodiscard]] static Result<SSTable> load(
        const std::filesystem::path& path,
        Arena& arena
    );

    [[nodiscard]] const std::filesystem::path& get_path() const;
    [[nodiscard]] const std::filesystem::path& get_final_path() const;

    [[nodiscard]] const SSTableEntities::FileHeaderSection&
        get_file_header_section() const;

    [[nodiscard]] const SSTableEntities::DataSection&
        get_data_section() const;

    [[nodiscard]] const SSTableEntities::DataSectionView&
        get_data_section_view() const;

    [[nodiscard]] const SSTableEntities::IndexSection&
        get_index_section() const;

    [[nodiscard]] const SSTableEntities::BloomSection&
        get_bloom_section() const;

    [[nodiscard]] const SSTableEntities::MetaSection&
        get_meta_section() const;

    [[nodiscard]] const SSTableEntities::FileFooterSection&
        get_file_footer_section() const;

    [[nodiscard]] Status append_record(const InternalRecord& record);

    [[nodiscard]] static std::size_t fixed_disk_size() noexcept;

    [[nodiscard]] Result<std::optional<InternalRecord>> get(
        const ArenaEntry& key,
        Arena& arena
    ) const;
};