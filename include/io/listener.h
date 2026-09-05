#pragma once

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>

#else

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#endif

#include <cerrno>
#include <optional>
#include <vector>

#ifdef _WIN32
using Socket = SOCKET;
using InvalidSocket = INVALID_SOCKET;
using SocketError = SOCKET_ERROR;
#else
using Socket = int;
using InvalidSocket = -1;
using SocketError = -1;
#endif


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
