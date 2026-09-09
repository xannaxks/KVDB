#include "worker.h"
class Worker
{
private:
	std::unique_ptr<KVDB> db_;
};