#pragma once

#include "io/socket.h"
#include "status.h"
#include <vector>
#include <cstdint>

class Connection
{	
	friend class ConnectionManager;

public:
	static constexpr std::size_t MAX_INPUT_BUFFER = 1 << 20; // 1 MB

	Connection(Socket fd);
	Connection(Socket fd, std::uint16_t buffer_reserve_size);
	Connection(Socket fd, std::uint16_t input_buffer_reserve_size, std::uint16_t output_buffer_reserve_size);
	
	Connection(const Connection&) = delete;
	Connection& operator=(const Connection&) = delete;

	Connection(Connection&& other) noexcept;

	bool operator==(const Connection& other);
	bool operator==(const int& other);

	~Connection();

	Status on_readable();
	Status on_writable();

	Status append_data(std::vector<std::byte>& data);

	Socket get_fd() const;
	Socket get_fd();
	

private:
	Socket fd_;

	std::vector<std::byte> input_buffer_;
	std::vector<std::byte> output_buffer_;

	void erase_sent_data(std::size_t offset);
};

struct ConnectionHash
{
	using is_transparent = void;

	std::size_t operator()(const Connection& c) const noexcept;

	std::size_t operator()(Socket fd) const noexcept;
};

struct ConnectionEqual
{
	using is_transparent = void;

	bool operator()(const Connection& a, const Connection& b) const noexcept;

	bool operator()(const Connection& a, Socket fd) const noexcept;

	bool operator()(Socket fd, const Connection& a) const noexcept;
};