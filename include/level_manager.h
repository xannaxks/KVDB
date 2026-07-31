/**
 * @file level_manager.h
 * @brief In-memory catalog of SSTables arranged into LSM levels.
 */
#pragma once

#include "table_meta.h"
#include "arena.h"
#include "status.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

/**
 * @brief Owns validated TableMeta entries for every configured level.
 *
 * L0 tables may overlap and are searched newest-first. Tables in L1 and above
 * are maintained in key-range order and must not overlap. Mutations validate
 * these invariants before publishing metadata.
 */
class LevelManager {
public:
    /** @brief Creates @p level_count levels, including L0. */
    explicit LevelManager(std::uint32_t level_count = 2);

    /** @brief Adds a table while preserving the destination level's invariants. */
    [[nodiscard]] Status add_table(TableMeta&& table);

    /** @brief Removes a table by id, optionally requiring a specific level. */
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
    /**
     * @brief Returns tables in @p level whose ranges may contain @p key.
     *
     * L0 may return several overlapping candidates. Higher levels return at
     * most one candidate when their non-overlap invariant holds.
     */
    Result<std::vector<TableMeta>> find_candidate_tables_in_level(
        std::uint32_t level,
        const ArenaEntry& key
    ) const;

    [[nodiscard]]
    /** @brief Returns all tables intersecting the inclusive key interval. */
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
