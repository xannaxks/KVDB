#pragma once

#include "connection.h"
#include <vector>
#include <cstdint>
#include <unordered_set>
#include <optional>

class ConnectionManager
{
public:
	ConnectionManager() = default;
	~ConnectionManager() = default;

	ConnectionManager(const ConnectionManager&) = delete;
	ConnectionManager& operator=(const ConnectionManager&) = delete;

	Status add(Socket fd);
	Status add(Connection&& connection);

	Status remove(Socket fd);

	const Connection* get(Socket fd) const;
	const Connection* get(Socket fd);

private:
	std::unordered_set<Connection, ConnectionHash, ConnectionEqual> connections_;
};