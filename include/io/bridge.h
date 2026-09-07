#include "socket.h"
#include <vector>
#include <cstdint>
#include <queue>
#include <mutex>

struct WorkerResult
{
	Socket fd;
	std::vector<std::byte> data;
};

class Bridge
{
public:
	Bridge();
	~Bridge();

	void push(WorkerResult& result);
	void push(WorkerResult&& result);

	std::vector<WorkerResult> drain();

	int get_event_fd() const;

private:
	std::vector<WorkerResult> results_;
	std::mutex mutex_;

	int event_fd_ = -1;
	bool has_data_ = false;
};