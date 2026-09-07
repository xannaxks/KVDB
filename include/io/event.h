#include "io/socket.h"
#include <cstdint>

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
