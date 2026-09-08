#include "io/bridge.h"
#include <assert.h>

#ifndef _WIN32

#include "sys/eventfd.h"

#endif

Bridge::Bridge()
	: has_data_(false)
{
#ifndef _WIN32
	event_fd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

	if (event_fd_ == -1)
	{
		int error = get_last_socket_error();
		throw std::system_error(
			error,
			std::system_category(),
			"failed to create eventfd"
		);
	}
#endif
}

Bridge::~Bridge()
{
	::close_socket_noexcept(event_fd_);
}

void Bridge::push(WorkerResult&& result)
{
	static constexpr std::uint64_t increment = 1;

	std::lock_guard<std::mutex> lock(mutex_);
	results_.emplace_back(std::move(result));

	if (has_data_)
		return;

	for (;;)
	{
#ifndef _WIN32
		ssize_t bytes_written = ::write(event_fd_, &increment, sizeof(increment));
#else
		int bytes_written;
#endif
		if (bytes_written == SocketError)
		{
			int last_error = ::get_last_socket_error();

			if (last_error == EINTR)
				continue;

			results_.pop_back(); // rolling back changes

			throw std::system_error(
				last_error,
				std::system_category(),
				"failed to write to event_fd"
			);
		}

		if (bytes_written == 8)
			break;

		int last_error = ::get_last_socket_error();

		results_.pop_back(); // rolling back changes

		throw std::system_error(
			last_error,
			std::system_category(),
			"attempt to write to event_fd returned 0 without appropriate error, last error provided"
		);
	}

	has_data_ = true;
}

void Bridge::push(WorkerResult& result)
{
	static constexpr std::uint64_t increment = 1;

	std::lock_guard<std::mutex> lock(mutex_);
	results_.emplace_back(result);

	if (has_data_)
		return;

	for (;;)
	{
#ifndef _WIN32
		ssize_t bytes_written = ::write(event_fd_, &increment, sizeof(increment));
#else
		int bytes_written;
#endif
		if (bytes_written == SocketError)
		{
			int last_error = ::get_last_socket_error();

			if (last_error == EINTR)
				continue;

			results_.pop_back(); // rolling back changes

			throw std::system_error(
				last_error,
				std::system_category(),
				"failed to write to event_fd"
			);
		}

		if (bytes_written == 8)
			break;

		int last_error = ::get_last_socket_error();

		results_.pop_back(); // rolling back changes

		throw std::system_error(
			last_error,
			std::system_category(),
			"attempt to write to event_fd returned 0 without appropriate error, last error provided"
		);
	}

	has_data_ = true;
}

std::optional<std::vector<WorkerResult>> Bridge::drain()
{
	static std::uint64_t buff = 0;

	std::vector<WorkerResult> drained_results;
	
	{
		std::lock_guard<std::mutex> lock(mutex_);

		drained_results.swap(results_);
		has_data_ = false;

		for (;;)
		{
			int read;

#ifndef _WIN32
			read = static_cast<int>(::read(this->event_fd_, &buff, sizeof(buff)));
#endif

			if (read == SocketError)
			{
				int last_error = get_last_socket_error();

				if (last_error == EINTR)
					continue;

				if (last_error == EAGAIN || last_error == EWOULDBLOCK)
				{
					results_.swap(drained_results);
					has_data_ = true;

					return std::nullopt;
				}


				results_.swap(drained_results);
				has_data_ = true;

				throw std::system_error(
					last_error,
					std::system_category(),
					"failed to read data from event_fd; "
				);
			}

			break; // can not result in partial reads
		}

	} // Lock RAII

	return drained_results;
}

int Bridge::get_event_fd() const
{
	return event_fd_;
}