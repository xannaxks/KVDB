#pragma once

#include <cerrno>
#include <optional>
#include <vector>

#include "socket.h"


class Listener
{
private:
	Socket fd_ = InvalidSocket;
	std::uint16_t port;

	void close_socket();
	void close_socket_noexcept() noexcept;

public:
	Listener(std::uint16_t port)
		: port(port)
	{};
	~Listener();

	Listener(const Listener&) = delete;
	Listener& operator=(const Listener&) = delete;

	/*
	* @brief Accept one pending connection.
	* @return The newly connected socket.
	*/
	std::optional<Socket> accept_connection();

	// @brief descriptor getters
	Socket get_fd() const noexcept;
};
