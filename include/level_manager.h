#pragma once

#include "table_meta.h"
#include "arena.h"
#include "status.h"
#include <cstdint>
#include <optional>
#include <vector>

class LevelManager {
public:
    LevelManager() noexcept;

    Status add_table(TableMeta&& table);
    Status remove_table(std::uint64_t table_id, std::optional<std::uint32_t> level = std::nullopt);

    const std::vector<TableMeta>* get_lx_tables(std::uint32_t level) const;
    std::uint32_t level_count() const noexcept;

    Result<std::vector<TableMeta>> find_candidate_tables_in_level(
        std::uint32_t level,
        const ArenaEntry& key
    ) const;

    std::vector<TableMeta> find_overlapping_tables(
        std::uint32_t level,
        const ArenaEntry& smallest,
        const ArenaEntry& largest
    ) const;

    const std::vector<TableMeta>& levels(std::size_t lvl) const;

    std::size_t get_layer_size(std::size_t lvl) const;

private:
    std::vector<std::vector<TableMeta>> levels_;
};
