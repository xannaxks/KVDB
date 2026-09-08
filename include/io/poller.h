#include "io/socket.h"
#include "io/connection.h"
#include "io/event.h"
#include "status.h"

#ifndef _WIN32

#include <sys/epoll.h>

#else

#endif

class Poller
{
public:
	static constexpr int MAX_EVENTS = 64;

	Poller();
	~Poller();

	// dont take tempoorary objects, cuz it will instantly destroy and close the socket
	Status register_fd(Connection& connection);
	Status remove_fd(Socket fd);

	Status alter_fd_events(Connection& connection, std::uint32_t new_events);

	std::vector<Event> wait();

	Socket get_fd() const;
	Socket get_fd();

private:
	Socket epoll_fd_ = InvalidSocket;
};