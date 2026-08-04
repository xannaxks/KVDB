# KVDB

KVDB is an experimental C++23 key-value storage engine based on the
Log-Structured Merge Tree architecture.

The project started as a mini LSM database implementation and is evolving into
an experimental platform for comparing different in-memory MemTable data
structures, such as Red-Black Trees, Skip Lists, AVL Trees, Treaps, and other
ordered indexes.

The main goal is to study how different MemTable implementations affect write
performance, read performance, memory usage, flushing behavior, and overall
storage-engine design trade-offs.

> KVDB is currently experimental and not intended for production use.

---

## Features

Current / planned components:

* [x] Write-Ahead Log
* [x] Arena allocator
* [x] Red-Black Tree MemTable
* [x] Skip List MemTable
* [x] SSTable file format
* [x] Table metadata
* [x] Level manager
* [x] Manifest / version edits
* [x] Point lookup API
* [x] Compaction
* [x] Bloom filter integration
* [ ] Background compaction
* [x] Benchmarks
* [ ] Additional MemTable data structures

---

## Project Goals

KVDB has two main goals:

1. **Storage engine implementation**

   Build a small but realistic LSM-based key-value database, including WAL,
   MemTables, SSTables, metadata management, and compaction.

2. **MemTable data structure research**

   Use the database as a controlled environment for comparing different
   in-memory ordered data structures and understanding their practical trade-offs.

The project focuses on correctness, clear architecture, benchmarking, and
documentation rather than production-level completeness.

---

## Architecture Overview

At a high level, KVDB follows a simplified LSM-tree design:

```text
Client writes
    |
    v
Write-Ahead Log
    |
    v
MemTable
    |
    v
Immutable MemTable
    |
    v
SSTable files
    |
    v
Levels / Compaction
```

Main components:

* **WAL** — provides durability for recent writes.
* **MemTable** — stores recent records in memory using a pluggable ordered data structure.
* **SSTable** — immutable sorted table stored on disk.
* **Manifest** — tracks database version changes and table metadata.
* **Level Manager** — manages SSTables across levels.
* **Compaction** — merges SSTables and removes obsolete records.

More detailed documentation is available in [`docs/`](./docs).

---

## MemTable Implementations

KVDB is designed to support multiple MemTable backends.

Currently implemented:

| Data Structure | Status      |
| -------------- | ----------- |
| Red-Black Tree | Implemented |
| Skip List      | Implemented |

Planned / experimental:

| Data Structure                   | Status   |
| -------------------------------- | -------- |
| AVL Tree                         | Planned  |
| Treap                            | Planned  |
| Splay Tree                       | Planned  |
| B+ Tree / Buffered Tree variants | Research |
| Hash-based structures            | Research |

Each implementation may be compared using:

* Insert throughput
* Point lookup performance
* Range scan performance
* Memory overhead
* Flush cost
* Cache behavior
* Implementation complexity

---

## Repository Structure

```text
kvdb/
├── src/                  # Core implementation
├── include/              # Public/internal headers
├── tests/                # Unit and integration tests
├── benchmarks/           # Performance benchmarks
├── docs/                 # Design and implementation docs
├── research.md           # Research direction and experiment plan
├── roadmap.md            # Project roadmap
├── security.md           # Security policy
├── contributing.md       # Contribution guide
├── code_of_conduct.md    # Community rules
└── README.md
```

---

## Build

Requirements:

* C++23 compatible compiler
* CMake
* GoogleTest

Build:

```bash
cmake -S . -B out/build
cmake --build out/build
```

Run tests:

```bash
ctest --test-dir out/build
```

---

## Example Usage

Basic usage example:

```cpp
#include "kvdb.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace
{
    void print_status(const Status& status)
    {
        if (status.is_ok()) {
            std::cout << "OK\n";
            return;
        }
        std::cout << "ERROR " << static_cast<unsigned>(status.code);
        if (!status.message.empty()) {
            std::cout << ": " << status.message;
        }
        std::cout << '\n';
    }
}

int main(int argc, char** argv)
{
    DBOptions options;
    options.db_path = argc > 1
        ? std::filesystem::path(argv[1])
        : std::filesystem::path("kvdb-data");

    Result<std::unique_ptr<KVDB>> opened = KVDB::open(options);
    if (!opened.is_ok()) {
        print_status(opened.status);
        return 1;
    }

    std::unique_ptr<KVDB> database = std::move(opened.value);
    std::string command;

    while (std::cin >> command) {
        if (command == "exit" || command == ".exit") {
            break;
        }
        if (command == "flush") {
            print_status(database->flush());
            continue;
        }
        if (command == "put") {
            std::string key;
            std::string value;
            if (!(std::cin >> key >> value)) {
                std::cerr << "put requires: put <key> <value>\n";
                return 2;
            }
            print_status(database->put(key, value));
            continue;
        }
        if (command == "get") {
            std::string key;
            if (!(std::cin >> key)) {
                std::cerr << "get requires: get <key>\n";
                return 2;
            }
            auto result = database->get(key);
            if (!result.is_ok()) {
                print_status(result.status);
            }
            else if (!result.value.has_value()) {
                std::cout << "NOT_FOUND\n";
            }
            else {
                std::cout << *result.value << '\n';
            }
            continue;
        }
        if (command == "delete" || command == "remove") {
            std::string key;
            if (!(std::cin >> key)) {
                std::cerr << "delete requires: delete <key>\n";
                return 2;
            }
            print_status(database->remove(key));
            continue;
        }
        if (command == "compact") {
            std::string begin;
            std::string end;
            if (!(std::cin >> begin >> end)) {
                std::cerr << "compact requires: compact <begin> <end>\n";
                return 2;
            }
            print_status(database->compact_range(begin, end));
            continue;
        }

        std::cout << "ERROR: unknown command\n";
    }

    const Status close_status = database->close();
    if (!close_status.is_ok()) {
        print_status(close_status);
        return 1;
    }
    return 0;
}

```

> The public API is still unstable and may change during development.

---

## Documentation

Important documents:

* [`Docs`](./docs/README.md)
* [`Benchmarks`](./benchmarks/README.md)
* [`Research`](./RESEARCH.md)
* [`Roadmap`](./ROADMAP.md)

---

## Research Direction

KVDB may be used as an experimental platform for studying MemTable data
structures inside LSM-based storage engines.

Possible research questions:

* How do different ordered MemTable structures affect write throughput?
* Which structures provide better range-scan performance?
* How much memory overhead does each structure introduce?
* How do implementation complexity and practical performance compare?
* Are theoretically attractive structures actually useful in a small storage engine?

The research part is not only about raw benchmarks. The goal is also to explain
the results and connect them to real storage-engine behavior.

---

## Current Status

KVDB is under active development.

The current focus is:

* Completing the core database API
* Finishing manifest/version management
* Implementing compaction
* Stabilizing SSTable iteration and lookup
* Adding benchmarks
* Expanding MemTable implementations
* Improving documentation

---

## Disclaimer

KVDB is an educational and experimental project. It is not production-ready and
should not be used for storing important data.
