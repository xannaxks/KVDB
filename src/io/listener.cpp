#include "io/listener.h"
#include "status.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <cstdint>
#include <iostream>
#include <system_error>

Listener::Listener(std::uint16_t port)
	: port(port)
{
	try
	{
		fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	}
	catch (const std::exception& e)
	{
		/**
		* @todo Consider making log file with errors or use initialization function to return appropriate status
		*/
		std::cerr << "Error: " << e.what() << std::endl;
		return;
	}
	
	if (fd_ == -1)
	{
		int current_error = errno;
		std::cerr << "Error: " << std::system_error(current_error, "Error, failed to create socket: ");
		return;
	}

	if(::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1)
	{
#ifdef _WIN32
		closesocket(fd_);
#else
		::close(fd_);
#endif
		return syscall_error(StatusCode::SocketOptionFailed, "failed to set socket options");
	}

	sockaddr_in address{};

	address.sin_family = AF_INET;
	address.sin_addrs.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(port);

	if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1)
	{
		this->close_socket();
		return syscall_error(StatusCode::SocketBindFailed, "failed to bind socket to port")
	}


	if (::listen(fd_, SOMAXCONN) == -1)
	{
		this->close_socket();
	}
}

void Listener::close_socket()
{
#ifdef _WIN32
	if (::closesocket(fd_) == SOCKET_ERROR)
	{
		const DWORD err = ::GetLastError();
		// @todo Consider outputing to log file.
		std::cerr << std::system_category().message(err) << std::endl;
		throw std::runtime_error(std::system_category().message(err));
	}
#else 
	if (::close(fd_) == -1)
	{
		int error = errno;
		
		if (error == EINTR)
			return;
		
		if (error == EBADF)
		{
			std::cerr << std::system_category().message(error) << std::endl;
			throw std::runtime_error(std::system_category().message(error));
		}
		
	}
#endif
}