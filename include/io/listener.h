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
#include <vector>

class Listener
{
private:

#ifdef _WIN32
	SOCKET fd_ = INVALID_SOCKET;
#else
	int fd_ = -1;
#endif

	std::uint16_t port;

	void close_socket();
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
	int accept();

	// @brief descriptor getters
#ifdef _WIN32
	const SOCKET& socket() const noexcept;
#else
	int fd() const noexcept;
#endif
};
