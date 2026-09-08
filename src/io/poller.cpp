#include "io/poller.h"

Poller::Poller()
{
#ifndef _WIN32
	epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);

	if (epoll_fd_ == InvalidSocket)
	{
		int last_error = ::get_last_socket_error();

		throw std::system_error(
			last_error,
			std::system_category(),
			"failed to create epoll instance"
		);
	}
#endif
}

Poller::~Poller()
{
	::close_socket_noexcept(this->epoll_fd_);
}

Status Poller::register_fd(Connection& connection)
{
#ifndef _WIN32

	// Don't enable EPOLLOUT on level triggering, use dynamic flag setting.
	epoll_event event{
		EPOLLIN | EPOLLRDHUP,
		{ .ptr = &connection }
	};

	if (::epoll_ctl(this->epoll_fd_, EPOLL_CTL_ADD, connection.get_fd(), &event) == -1)
	{
		int last_error = ::get_last_socket_error();

		if (last_error == EEXIST)
			return Status{ StatusCode::Duplicate, "File descriptor already registered" };

		throw std::system_error(
			last_error,
			std::system_category(),
			"failed to add fd to epoll"
		);
	}

	return Status::ok();
#endif
}

Status Poller::remove_fd(Socket fd)
{
#ifndef _WIN32

	/*
	* Note:
	*	Before Linux 2.6.9, the EPOLL_CTL_DEL operation required a non-
	*	null pointer in event, even though this argument is ignored.
	*	Since Linux 2.6.9, event can be specified as NULL when using
	*	EPOLL_CTL_DEL.  Applications that need to be portable to kernels
	*	before Linux 2.6.9 should specify a non-null pointer in event
	*/
	if (::epoll_ctl(this->epoll_fd_, EPOLL_CTL_DEL, fd, NULL) == -1)
	{
		int last_error = ::get_last_socket_error();

		if (last_error == ENOENT)
			return Status{ StatusCode::NotFound, "File descriptor not found" };

		throw std::system_error(
			last_error,
			std::system_category(),
			"failed to remove fd from epoll"
		);
	}

	return Status::ok();
#endif
}

std::vector<Event> Poller::wait()
{
#ifndef _WIN32
	for (;;)
	{
		epoll_event events[MAX_EVENTS];
		int num_events = ::epoll_wait(this->epoll_fd_, events, MAX_EVENTS, -1);

		if (num_events == -1)
		{
			int last_error = ::get_last_socket_error();

			if (last_error == EINTR)
				continue;

			throw std::system_error(
				last_error,
				std::system_category(),
				"failed to wait for epoll events"
			);
		}

		std::vector<Event> result;
		result.reserve(num_events);

		for (int i = 0; i < num_events; ++i)
		{
			EventFlag flags = EventFlag::None;
			if (events[i].events & EPOLLIN)
				flags = static_cast<EventFlag>(static_cast<std::uint32_t>(flags) | static_cast<std::uint32_t>(EventFlag::Readable));
			if (events[i].events & EPOLLOUT)
				flags = static_cast<EventFlag>(static_cast<std::uint32_t>(flags) | static_cast<std::uint32_t>(EventFlag::Writable));
			if (events[i].events & EPOLLERR)
				flags = static_cast<EventFlag>(static_cast<std::uint32_t>(flags) | static_cast<std::uint32_t>(EventFlag::Error));
			if (events[i].events & EPOLLHUP)
				flags = static_cast<EventFlag>(static_cast<std::uint32_t>(flags) | static_cast<std::uint32_t>(EventFlag::Hangup));
			if (events[i].events & EPOLLRDHUP)
				flags = static_cast<EventFlag>(static_cast<std::uint32_t>(flags) | static_cast<std::uint32_t>(EventFlag::RemoteHangup));
			
			Connection* connection = reinterpret_cast<Connection*>(events[i].data.ptr);
			result.push_back({ connection->get_fd(), flags });
		}

		return result;
	}
#endif
}

Status Poller::alter_fd_events(Connection& connection, std::uint32_t new_events)
{
#ifndef _WIN32

	epoll_event event{
		new_events,
		{ .ptr = &connection }
	};

	if (::epoll_ctl(this->epoll_fd_, EPOLL_CTL_MOD, connection.get_fd(), &event) == -1)
	{
		int last_error = ::get_last_socket_error();

		if (last_error == ENOENT)
			return Status{ StatusCode::NotFound, "File descriptor not found" };

		throw std::system_error(
			last_error,
			std::system_category(),
			"failed to modify fd events in epoll"
		);
	}
	
	return Status::ok();
#endif
}

Socket Poller::get_fd() const
{
	return this->epoll_fd_;
}

Socket Poller::get_fd()
{
	return this->epoll_fd_;
}