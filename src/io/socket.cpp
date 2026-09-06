#include "io/socket.h"

int get_last_socket_error()
{
#ifdef _WIN32
	return static_cast<int>(::WSAGetLastError());
#else
	return errno;
#endif
}

void close_socket(Socket& fd_)
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

void close_socket_noexcept(Socket& fd_) noexcept
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

#ifdef _WIN32

int socket_send(Socket fd, const std::byte* data, std::size_t size) noexcept
{
	int result = ::send(fd, reinterpret_cast<const char*>(data), static_cast<int>(size), 0);

	return result;
}

int socket_receive(Socket fd, std::byte* buffer, std::size_t size)
{
	int bytes_received = ::recv(fd, reinterpret_cast<char*>(buffer), static_cast<int>(sizeof(buffer)), 0);

	return bytes_received;
}

#else

int socket_send(Socket fd, const std::byte* data, std::size_t size) noexcept
{
	ssize_t result = ::send(fd, data, size, MSG_NOSIGNAL);

	return static_cast<int>(result);
}

int socket_receive(Socket fd, std::byte* buffer, std::size_t size)
{

	ssize_t bytes_received = ::recv(fd, buffer, sizeof(buffer), 0);

	return static_cast<int>(bytes_received);
}

#endif