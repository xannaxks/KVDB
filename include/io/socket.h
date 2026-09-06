#pragma once


#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

using Socket = SOCKET;
inline constexpr Socket InvalidSocket = INVALID_SOCKET;
inline constexpr int SocketError = SOCKET_ERROR;

#else

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

using Socket = int;
inline constexpr Socket InvalidSocket = -1;
inline constexpr int SocketError = -1;

#endif

#include <cstdint>
#include <cerrno>
#include <stdexcept>
#include <system_error>

int get_last_socket_error();

void close_socket(Socket& socket);

void close_socket_noexcept(Socket& socket) noexcept;

int socket_send(Socket socket, const std::byte* buffer, std::size_t length);

int socket_receive(Socket socket, std::byte* buffer, std::size_t length);