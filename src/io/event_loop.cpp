#include "io/event_loop.h"
#include <assert.h>

#ifndef _WIN32

#include <sys/epoll.h>

#else

#endif

void EventLoop::remove(Socket socket) noexcept
{
	try
	{
		poller_.remove_fd(socket);
	}
	catch (...)
	{
	}

	try
	{
		connection_manager_.remove(socket);
	}
	catch (...)
	{
	}
}

Status EventLoop::handle_event(const Event& event)
{
	if (event.fd == bridge_.get_event_fd())
		return this->handle_bridge(event);
	else if (event.fd == listener_.get_fd())
		return this->handle_listener(event);
	else if (connection_manager_.get(event.fd) != nullptr)
		return this->handle_connection(event);
	else
		return Status{
			StatusCode::InvariantViolation,
			"event fd is not registered anywhere"
		};
}

Status EventLoop::handle_bridge(const Event& event)
{
	if (event.flags ^ EventFlag::Readable)
	{
		// EPOLLHUP and EPOLLRDHUP are not possible.
		// EPOLLHUP is not possible cuz eventfd had only one end.
		// EPOLLRDUP is not possible cuz its socket unique.
		// Only possible is EPOLLERR, usually means fata/critical error in kernel.
		throw std::system_error(
			"epoll over eventfd returned EPOLLERR, possible fatal/critical error in kernel."
		);
	}
	assert(event.flags == EventFlag::Readable); // EPOLLOUT was never registered

	std::vector<WorkerResult> results = bridge_.drain();

	return this->handle_results(results);
}

Status EventLoop::handle_results(const std::vector<WorkerResult>& results)
{
	bool partial = false;

	for (auto& result : results)
	{
		const Connection* connection = connection_manager_.get(result.fd);

		if (connection == nullptr)
		{
			// @note log into file
			partial = true;
			continue;
		}

		try
		{
			connection->append_data(result.data);
			this->poller_.alter_fd_events(result.fd, EPOLLIN | EPOLLOUT | EPOLLRDHUP);
		}
		catch (...)
		{
			// @note log into file.
			this->remove(connection->fd);
			partial = true;
		}
	}

	if (!partial)
		return Status::ok();

	return Status{ StatusCode::PartialOperation, "Some of operations failed and connections were closed" };
}

Status EventLoop::handle_listener(const Event& event)
{
	EventFlag flags = event.flags;

	bool partial = false; // only part of all connections were estabilished

	if (flags & EventFlag::Readable)
	{
		for (;;)
		{
			std::optional<Socket> new_socket = listener_.accept_connection();

			if (new_socket == std::nullopt)
				break;

			try
			{
				Status result = connection_manager_.add(*new_socket); // obtains ownership
			}
			catch (...)
			{
				// @todo add logger for failed connections
				::close_socket_noexcept(*new_socket);
				partial = true;
			}
		}
	}

	assert(!(flags & EventFlag::Writable));

	if (flags & (EventFlag::Error | EventFlag::Hangup | EventFlag::RemoteHangup))
	{
		try
		{
			poller_.remove_fd(listener_.get_fd());
			listener_.change_socket();
			poller_.register_fd(listener_.get_fd());
		}
		catch (const std::system_error& error)
		{
			throw std::system_error(
				error.code(),
				error.code().category(),
				"failed to replace listener socket: " + std::string(error.what())
			);
		}
	}

	if(!partial)
		return Status::ok();

	return Status{ StatusCode::PartialOperation, "Only part of all connections were estabilished" };
}

Status EventLoop::handle_connection(const Event& event)
{
	EventFlag flags = event.flags;
	const Connection* connection = nullptr;

	try {
		// normally .get shouldn't throw, cuz it uses unordered_set::find underhood that doesn't throw itself,
		// except cases when Hash or KeyEqual throw, but our Hash uses std::hash which again doesn't throw and 
		// simple operators for KeyEqual.
		connection = connection_manager_.get(event.fd);
	}
	catch (...)
	{
		// removes from everywhere, including connection manager that holds ownership and will close the socket
		this->remove(event.fd);
		throw;
	}

	if (connection == nullptr)
		return Status{ StatusCode::NotFound, "event.fd not found in connection manager" };

	if (flags & EventFlag::Readable)
	{
		try {
			Status result = connection->on_readable();

			if (result.code == StatusCode::ConnectionClosed
				|| result.code == StatusCode::BufferTooSmall)
			{
				this->remove(event.fd);
				return result;
			}

			//if (result.code != StatusCode::WouldBlock)
			//	poller_.alter_fd_events(event.fd, EPOLLIN | EPOLLOUT | EPOLLRDHUP);
		}
		catch (...)
		{
			this->remove(event.fd);
			throw;
		}
	}

	if (flags & EventFlag::Writable)
	{
		try
		{
			// on writable can throw
			Status result = connection->on_writable();

			if (result.code == StatusCode::ConnectionClosed)
			{
				this->remove(event.fd);
				return result;
			}
			
			if (result.code != StatusCode::WouldBlock)
			{
				assert(result.is_ok());
				// can throw
				poller_.alter_fd_events(event.fd, EPOLLIN | EPOLLRDHUP);
			}
		}
		catch (...)
		{
			this->remove(event.fd);
			throw;
		}
	}

	if (flags & EventFlag::Error)
	{
		this->remove(event.fd);
		return Status{
			StatusCode::ConnectionClosed, "connection was closed due to error event"
		};
	}
	if (flags & EventFlag::Hangup)
	{
		this->remove(event.fd);
		return Status{
			StatusCode::ConnectionClosed,
			"connection was closed due to hangup event"
		};
	}
	if (flags & EventFlag::RemoteHangup)
	{
		this->remove(event.fd);
		return Status{
			StatusCode::ConnectionClosed,
			"connection was closed due to remote hangup event"
		};
	}

	return Status::ok();
}

void EventLoop::run()
{
	for (;;)
	{
		std::vector<Event> events = poller_.wait();
		for (const auto& event : events)
		{
			Status result = this->handle_event(event);
		}
	}
}