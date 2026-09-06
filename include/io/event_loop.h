#pragma once

#include "connection_manager.h"
#include "listener.h"

enum class EventFlag : std::uint32_t
{
	None = 0,
	Readable = 1 << 0, // EPOLLIN
	Writable = 1 << 1, // EPOLLOUT
	Error = 1 << 2, // EPOLLERR
	Hangup = 1 << 3, // EPOLLHUP
};

struct Event
{
	Socket fd;
	EventFlag flags;
};

class EventLoop
{
public:
	void run(); // delegates to poller_ and handles events

	void handle_event(const Event& event);

	void handle_eventfd();
	void handle_on_readable(Socket fd);
	void handle_on_writable(Socket fd);
	void handle_accept();
	void handle_error(Socket fd);

private:

	// epolls, don't own and dont know anythning about fds, just wait for events and delegate handling to event_loop
	Poller poller_;

	Bridge eventfd_;
	Listener listener_;
	ConnectionManager connection_manager_;
};