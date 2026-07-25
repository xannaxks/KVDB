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
