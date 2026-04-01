#pragma once
#include "mem_table.h"
#include "wal.h"
#include "type.h"
#include <filesystem>
#include "sstable.h"

namespace EnvVariables
{
    const std::string sstable_dir = "sstable_dir";
    uint32_t sstable_num = 0;
};

class Engine
{
public:

    enum class Status
    {
        OK,
        KeyNotFound,
        KeyWasDeleted,
        MemoryAllocationFailed,
        EntryTooLarge,
        EndOfFile,
    };

private:
    MemTable mem_table;
    Wal wal;
    uint64_t seq_cnt = 0;
    std::vector<std::unique_ptr<SSTable>> sstables;

    static Engine::Status status_from(MemTable::Status);
    static Engine::Status status_from(Wal::Status);
    void load_wal();
    void ensure_sstable_dir();
    void ensure_dirs();
    void load_sstables();

public:

    explicit Engine(const std::string& path);

    void start_up();

    Engine::Status put(const std::string& key, const std::string& value);
    Engine::Status remove(const std::string& key);

    Engine::Status manual_freeze();

    std::variant<std::string, Engine::Status> get(const std::string& key);

    void manual_flush();
};