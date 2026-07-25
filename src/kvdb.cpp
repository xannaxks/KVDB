#include "kvdb.h"

#include "engine.h"

#include <new>
#include <utility>

Result<std::unique_ptr<KVDB>> KVDB::open(const DBOptions& options)
{
    try {
        auto engine = std::make_unique<Engine>(options);
        Status status = engine->open();
        if (!status.is_ok()) {
            return Result<std::unique_ptr<KVDB>>::fail(
                std::move(status)
            );
        }

        std::unique_ptr<KVDB> database = std::move(engine);
        return Result<std::unique_ptr<KVDB>>::ok(
            std::move(database)
        );
    }
    catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<KVDB>>::fail(Status{
            StatusCode::OutOfMemory,
            "could not allocate the database engine"
        });
    }
    catch (const std::exception& exception) {
        return Result<std::unique_ptr<KVDB>>::fail(Status{
            StatusCode::InternalError,
            exception.what()
        });
    }
}
