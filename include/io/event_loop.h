#pragma once

#include "connection_manager.h"
#include "listener.h"

class EventLoop
{
public:
	void run();

private:
	int epoll_fd_;

	Listener listener_;
	ConnectionManager connection_manager_;
};