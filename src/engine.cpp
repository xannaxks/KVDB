#include "engine.h"
#include <cassert>
#include <iostream>
#include <format>
#include <algorithm>

using namespace EnvVariables;

Engine::Status Engine::status_from(MemTable::Status mem_table_status)
{
    switch (mem_table_status)
    {
    case MemTable::Status::OK:
        return Engine::Status::OK;
    case MemTable::Status::KeyNotFound:
        return Engine::Status::KeyNotFound;
    case MemTable::Status::KeyWasDeleted:
        return Engine::Status::KeyWasDeleted;
    case MemTable::Status::MemoryAllocationFailed:
        return Engine::Status::MemoryAllocationFailed;
    }

    assert(false);
    return Engine::Status::MemoryAllocationFailed;
}
Engine::Status Engine::status_from(Wal::Status wal_status)
{
    switch (wal_status)
    {
    case Wal::Status::OK:
        return Engine::Status::OK;
    case Wal::Status::EntryTooLarge:
        return Engine::Status::EntryTooLarge;
    case Wal::Status::EndOfFile:
        return Engine::Status::EndOfFile;
    }

    assert(false);
    return Engine::Status::MemoryAllocationFailed;
}

Engine::Engine(const std::string& path)
    : mem_table(),
    wal(path)
{
}

void Engine::ensure_sstable_dir()
{
    if (!std::filesystem::exists(sstable_dir))
        std::filesystem::create_directory(sstable_dir);
}
void Engine::ensure_dirs()
{
    ensure_sstable_dir();
}
void Engine::load_wal()
{
    while (true)
    {
        std::variant<Wal::Status, InternalRecord> entry = wal.read_next();

        if (std::holds_alternative<Wal::Status>(entry))
        {
            if (std::get<Wal::Status>(entry) == Wal::Status::EndOfFile)
                break;
            assert(false);
        }
        InternalRecord record = std::get<InternalRecord>(entry);
        mem_table.apply(record);
        seq_cnt = std::max(seq_cnt, record.seq_num);
    }
    seq_cnt++;
}
void Engine::load_sstables()
{
    int cnt = 1;
    while (true)
    {
        std::string path = std::format("{}/{:05}.sst", sstable_dir, cnt);
        if (!std::filesystem::exists(path))
        {
            break;
        }
        this->sstables.push_back(std::make_unique<SSTable>(path));
        cnt++;
    }
    sstable_num = cnt;
    std::reverse(this->sstables.begin(), this->sstables.end());
}
void Engine::start_up()
{
    ensure_dirs();
    load_wal();
    load_sstables();
}

Engine::Status Engine::put(const std::string& key, const std::string& value)
{
    Bytes key_buffer, value_buffer;
    ::write_to_bytes(key_buffer, key);
    ::write_to_bytes(value_buffer, value);

    Wal::Entry entry(key_buffer, value_buffer, Type::Put, seq_cnt);
    auto status = wal.write(entry);
    if (status != Wal::Status::OK)
        return Engine::status_from(status);

    seq_cnt++;

    return Engine::status_from(
        mem_table.put(entry.payload.key, entry.payload.value, entry.header.seq_num)
    );
}
Engine::Status Engine::remove(const std::string& key)
{
    Bytes key_buffer;
    ::write_to_bytes(key_buffer, key);

    Wal::Entry entry(key_buffer, Bytes(), Type::Tombstone, seq_cnt);
    auto status = wal.write(entry);
    if (status != Wal::Status::OK)
        return Engine::status_from(status);
    
    seq_cnt++;

    return Engine::status_from(
        mem_table.remove(entry.payload.key, entry.header.seq_num)
    );
}
Engine::Status Engine::manual_freeze()
{
    return Engine::status_from(mem_table.manual_freeze());
}
std::variant<std::string, Engine::Status> Engine::get(const std::string& key)
{
    Bytes key_buffer;
    ::write_to_bytes(key_buffer, key);

    auto result = mem_table.get(key_buffer);

    if (std::holds_alternative<ByteRecord>(result))
    {
        std::string value;
        write_to_string(value, std::get<ByteRecord>(result).value);
        return value;
    }

    if (std::get<MemTable::Status>(result) == MemTable::Status::KeyWasDeleted)
        return Engine::Status::KeyWasDeleted;

    for (auto& sstable : this->sstables)
    {
        auto sstable_result = sstable->get(key_buffer);
        if (std::holds_alternative<ByteRecord>(sstable_result))
        {
            std::string value;
            write_to_string(value, std::get<ByteRecord>(sstable_result).value);
            return value;
        }
        if (std::get<SSTable::Status>(sstable_result) == SSTable::Status::KeyWasDeleted)
            return Engine::Status::KeyWasDeleted;
    }
    return Engine::Status::KeyNotFound;
}
void Engine::manual_flush()
{
    SSTableBuilder::build_from_memtable(this->mem_table, sstable_dir, sstable_num);
}