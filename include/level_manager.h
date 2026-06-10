#include "table_meta.h"
#include "arena.h"
#include "status.h"
#include <vector>

class LevelManager {
public:
    LevelManager() noexcept;

    Status add_table(TableMeta&& table);
    Status remove_table(std::uint64_t table_id, std::optional<std::uint32_t> level);

    const std::vector<TableMeta>* get_lx_tables(std::uint32_t level) const;

    Result<std::vector<TableMeta>> find_candidate_tables_in_level(
        std::uint32_t level,
        const ArenaEntry& key
    ) const;

    std::vector<TableMeta> find_overlapping_tables(
        std::uint32_t level,
        const ArenaEntry& smallest,
        const ArenaEntry& largest
    ) const;

private:
    std::vector<std::vector<TableMeta>> levels_;
};