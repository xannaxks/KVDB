#pragma once

#include "table_meta.h"
#include "arena.h"
#include "status.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

class LevelManager {
public:
    // Pass DBOptions::max_levels here. A database must have at least L0.
    explicit LevelManager(std::uint32_t level_count = 2);

    [[nodiscard]] Status add_table(TableMeta&& table);

    [[nodiscard]] Status remove_table(
        std::uint64_t table_id,
        std::optional<std::uint32_t> level = std::nullopt
    );

    [[nodiscard]]
    const std::vector<TableMeta>* get_lx_tables(
        std::uint32_t level
    ) const noexcept;

    [[nodiscard]] std::uint32_t level_count() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]]
    Result<std::vector<TableMeta>> find_candidate_tables_in_level(
        std::uint32_t level,
        const ArenaEntry& key
    ) const;

    [[nodiscard]]
    std::vector<TableMeta> find_overlapping_tables(
        std::uint32_t level,
        const ArenaEntry& smallest,
        const ArenaEntry& largest
    ) const;

    [[nodiscard]]
    const std::vector<TableMeta>& levels(std::size_t level) const;

    [[nodiscard]]
    std::size_t get_layer_size(std::size_t level) const;

    void swap(LevelManager& other) noexcept;

private:
    std::vector<std::vector<TableMeta>> levels_;
};