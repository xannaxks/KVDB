#pragma once

#include "connection.h"
#include <vector>
#include <byte>
#include <unordered_map>

class ConnectionManager
{
public:
	void add(int fd);
	void remove(int fd);

	Connection& get(int fd);

private:
	std::unordered_map<int, Connection> connections_;
};