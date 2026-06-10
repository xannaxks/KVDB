class LevelManager {
public:
    Status add_table(TableMeta table);
    Status remove_table(std::uint64_t table_id);

    std::vector<TableMeta> get_l0_tables_newest_first() const;

    Result<std::optional<TableMeta>> find_table_in_level(
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