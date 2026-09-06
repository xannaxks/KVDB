#include "io/connection_manager.h"
#include <assert.h>

Status ConnectionManager::add(Socket fd)
{
    auto result = this->connections_.find(fd);

    if (result != this->connections_.end())
        return Status{
            StatusCode::Duplicate,
            "attempt to insert already existing fd/connection"
        };

    auto [it, inserted] =
        connections_.emplace(fd);
    
    assert(inserted);

    return Status::ok();
}

Status ConnectionManager::add(Connection&& connection)
{
    auto [it, inserted] =
        connections_.insert(std::move(connection));

    if (!inserted)
        return Status{
            StatusCode::Duplicate,
            "attempt to insert already existing fd/connection"
    };

    return Status::ok();
}

const Connection* ConnectionManager::get(Socket fd) const
{
    auto it = connections_.find(fd);

    if (it == connections_.end())
        return nullptr;

    return std::addressof(*it);
}

const Connection* ConnectionManager::get(Socket fd)
{
    auto it = connections_.find(fd);

    if (it == connections_.end())
        return nullptr;

    return std::addressof(*it);
}

Status ConnectionManager::remove(Socket fd)
{
    auto it = connections_.find(fd);

    if (it == connections_.end())
        return Status{
            StatusCode::NotFound,
            "the element to be erased is not found"
    };

    connections_.erase(it); // throws nothing

    return Status::ok();
}