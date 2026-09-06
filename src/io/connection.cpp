#include "io/connection.h"
#include <functional>

Connection::Connection(Socket fd)
	: fd_(fd)
{}

Connection::Connection(Socket fd, std::uint16_t buffer_reserve_size)
	: fd_(fd)
{
	input_buffer_.reserve(buffer_reserve_size);
	output_buffer_.reserve(buffer_reserve_size);
}

Connection::Connection(Socket fd, std::uint16_t input_buffer_reserve_size, std::uint16_t output_buffer_reserve_size)
	: fd_(fd)
{
	input_buffer_.reserve(input_buffer_reserve_size);
	output_buffer_.reserve(output_buffer_reserve_size);
}

Connection::~Connection()
{
	::close_socket_noexcept(fd_);
}

bool Connection::operator==(const Connection& other)
{
	return this->fd_ == other.fd_;
}

bool Connection::operator==(const int& other)
{
	return other == this->fd_;
}

Connection::Connection(Connection&& other) noexcept
	: fd_(std::exchange(other.fd_, InvalidSocket)),
	input_buffer_(std::move(other.input_buffer_)),
	output_buffer_(std::move(other.output_buffer_))
{
}

void Connection::erase_sent_data(std::size_t offset)
{
	if (offset != 0)
	{
		output_buffer_.erase(
			output_buffer_.begin(),
			output_buffer_.begin() + offset
		);
	}
}

Status Connection::on_writable()
{
	std::size_t offset = 0;

	while (offset < output_buffer_.size())
	{
		std::byte* data = output_buffer_.data() + offset;
		std::size_t bytes_to_send =
			output_buffer_.size() - offset;

		int bytes_sent = socket_send(fd_, data, bytes_to_send);

		if (bytes_sent == SocketError)
		{
			int error = ::get_last_socket_error();

#ifdef _WIN32

			if (error == WSAEINTR)
				continue;

			if (error == WSAEWOULDBLOCK)
			{
				erase_sent_data(offset);

				return {
					StatusCode::WouldBlock,
					"socket not ready for writing"
				};
			}

			if (error == WSAECONNRESET ||
				error == WSAECONNABORTED ||
				error == WSAENOTCONN)
			{
				return {
					StatusCode::ConnectionClosed,
					"connection closed by the peer"
				};
			}

#else

			if (error == EINTR)
				continue;

			if (error == EWOULDBLOCK ||
				error == EAGAIN)
			{
				erase_sent_data(offset);

				return {
					StatusCode::WouldBlock,
					"socket not ready for writing"
				};
			}

			if (error == EPIPE ||
				error == ECONNRESET) // RAII will close the fd_, so we don't need to call close_socket() here
			{
				return {
					StatusCode::ConnectionClosed,
					"connection closed by the peer"
				};
			}

#endif

			throw std::system_error(
				error,
				std::system_category(),
				"failed to send data"
			);
		}

		if (bytes_sent == 0)
			break;

		offset += static_cast<std::size_t>(bytes_sent);
	}


	/// @todo Consider using a more efficient method to remove sent data from the output buffer,
	/// such as using a circular buffer or a deque, to avoid the overhead of erasing elements from the beginning of a vector.
	erase_sent_data(offset);

	return Status::ok();
}

Status Connection::on_readable()
{
	while (true)
	{
		std::byte buffer[4096];

		int bytes_received = socket_receive(fd_, buffer, sizeof(buffer));

		if (bytes_received == SocketError)
		{
			int current_error = ::get_last_socket_error();

#ifdef _WIN32

			if (current_error == WSAEINTR)
				continue;

			if (current_error == WSAEWOULDBLOCK)
			{
				// fd_ is not ready for reading.
				return {
					StatusCode::WouldBlock,
					"fd_ not ready for reading"
				};
			}

#else

			if (current_error == EINTR)
				continue;

			if (current_error == EWOULDBLOCK ||
				current_error == EAGAIN)
			{
				// fd_ is not ready for reading.
				return {
					StatusCode::WouldBlock,
					"fd_ not ready for reading"
				};
			}

#endif

			throw std::system_error(
				current_error,
				std::system_category(),
				"failed to receive data"
			);
		}

		if (bytes_received == 0)
		{
			// Peer performed an orderly shutdown.
			// Connection owns fd_, so RAII will close it.
			return {
				StatusCode::ConnectionClosed,
				"connection closed by the peer"
			};
		}

		const std::size_t received =
			static_cast<std::size_t>(bytes_received);

		if (input_buffer_.size() + received > MAX_INPUT_BUFFER)
		{
			return {
				StatusCode::BufferTooSmall,
				"input buffer overflow"
			};
		}

		input_buffer_.insert(
			input_buffer_.end(),
			buffer,
			buffer + received
		);
	}

	return Status::ok();
}

Socket Connection::get_fd() const
{
	return this->fd_;
}

Socket Connection::get_fd()
{
	return this->fd_;
}

std::size_t ConnectionHash::operator()(const Connection& c) const noexcept
{
	return std::hash<Socket>{}(c.get_fd());
}

std::size_t ConnectionHash::operator()(Socket fd) const noexcept
{
	return std::hash<Socket>{}(fd);
}

bool ConnectionEqual::operator()(const Connection& a, const Connection& b) const noexcept
{
	return a.get_fd() == b.get_fd();
}

bool ConnectionEqual::operator()(const Connection& a, Socket fd) const noexcept
{
	return a.get_fd() == fd;
}

bool ConnectionEqual::operator()(Socket fd, const Connection& a) const noexcept
{
	return fd == a.get_fd();
}