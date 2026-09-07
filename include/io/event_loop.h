#pragma once

#include "io/connection_manager.h"
#include "io/listener.h"
#include "io/event.h"
#include "io/poller.h"
#include "io/bridge.h"

class EventLoop
{
public:
	void run(); // delegates to epolling to poller_, handles events itself

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