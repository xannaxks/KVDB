#pragma once
#include "status.h"
#include <memory>
#include <optional>
#include "db_options.h"

class KVDB
{
public:
    virtual ~KVDB() = default;

    static Result<std::unique_ptr<KVDB>> open(const DBOptions& options);

    virtual Status put(std::string_view key, std::string_view value) = 0;

    virtual Result<std::optional<std::string>> get(std::string_view key) = 0;

    virtual Status remove(std::string_view key) = 0;

    virtual Status flush() = 0;

    virtual Status compact_range(std::string_view begin,
        std::string_view end) = 0;

    virtual Status close() = 0;

protected:
    KVDB() = default;
};