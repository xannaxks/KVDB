#include "io/listener.h"
#include "status.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <cstdint>
#include <iostream>
#include <system_error>
#include <optional>

Listener::Listener(std::uint16_t port)
	: port(port)
{
#ifdef _WIN32
	fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
	fd_ = ::socket(
		AF_INET,
		SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
		0
	);
#endif

	if (fd_ == InvalidSocket)
	{
		int current_error = get_last_socket_error();

		throw std::system_error(
			current_error,
			std::system_category(),
			"failed to create socket"
		);
	}

#ifdef _WIN32
	u_long mode = 1;
	
	if (::ioctlsocket(fd_, FIONBIO, &mode) == SocketError)
	{
		int current_error = get_last_socket_error();

		this->close_socket_noexcept();

		throw std::system_error(
			current_error,
			std::system_category(),
			"failed to switch socket to non blocking mode"
		);
	}
#endif

	const char yes = 1;

	if(::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == SocketError)
	{
		const int current_error = get_last_socket_error();

		this->close_socket_noexcept();

		throw std::system_error(
			current_error,
			std::system_category(),
			"failed to set socket options"
		);
	}

	sockaddr_in address{};

	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(port);

	if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SocketError)
	{
		const int current_error = get_last_socket_error();

		this->close_socket_noexcept();

		throw std::system_error(
			current_error,
			std::system_category(),
			"failed to bind socket to address"
		);
	}

	if (::listen(fd_, SOMAXCONN) == SocketError)
	{
		const int current_error = get_last_socket_error();

		this->close_socket_noexcept();

		throw std::system_error(
			current_error,
			std::system_category(),
			"failed to set socket to listen mode"
		);
	}
}

void Listener::close_socket()
{
	if (fd_ == InvalidSocket)
		return;

#ifdef _WIN32
	const int result = ::closesocket(fd_);
#else
	const int result = ::close(fd_);
#endif

	if (result == -1)
	{
		const int error = get_last_socket_error();

		fd_ = InvalidSocket;

#ifndef _WIN32
		if (error == EINTR)
			return;
#endif

		throw std::system_error(
			error,
			std::system_category(),
			"failed to close socket"
		);
	}

	fd_ = InvalidSocket;
}

void Listener::close_socket_noexcept() noexcept
{
	if (fd_ == InvalidSocket)
		return;

	// consider logging into logging system in case of failure, but don't throw
#ifdef _WIN32
	::closesocket(fd_);
#else
	::close(fd_);
#endif

	fd_ = InvalidSocket;
}

Listener::~Listener()
{
	this->close_socket_noexcept();
}

Socket Listener::get_fd() const noexcept
{
	return this->fd_;
}

int get_last_os_error()
{
#ifdef _WIN32
	return static_cast<int>(::GetLastError());
#else
	return errno;
#endif
}

int get_last_socket_error()
{
#ifdef _WIN32
	return static_cast<int>(::WSAGetLastError());
#else
	return errno;
#endif
}

std::optional<Socket> Listener::accept_connection()
{
#ifdef _WIN32
	Socket client = ::accept(this->fd_, nullptr, nullptr);
	
	if (client == InvalidSocket)
	{
		int current_error = get_last_socket_error();

		if (current_error == WSAEWOULDBLOCK)
			return std::nullopt;

		throw std::system_error(
			current_error,
			std::system_category(),
			"failed to estabilish connection with client"
		);
	}

	u_long mode = 1;

	if (::ioctlsocket(client, FIONBIO, &mode) == SocketError)
	{
		int current_error = get_last_socket_error();

		::closesocket(client); // we dont care about potential errors produced by closesocket, cuz we throw top layer error

		throw std::system_error(
			current_error,
			std::system_category(),
			"failed to switch connected socket mode"
		);
	}

	return client;
#else
	for (;;) { // outter infinite loop, needd to handle EINTR
		Socket client = ::accept4(fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);

		if (client == InvalidSocket)
		{
			int current_error = get_last_socket_error();

			// nothing, probably was interrupted by sys call, not really exception in ordinary way
			if (current_error == EINTR)
				continue;

			if (current_error == EAGAIN || current_error == EWOULDBLOCK) // no client waiting to estabilish connection
				return std::nullopt; // returning std::nullopt in case if no client waiting to establish connection, in case of failure - throw

			throw std::system_error(
				current_error,
				std::system_category(),
				"failed to estabilish connection with client"
			);
		}

		return client;
	}
#endif
}

