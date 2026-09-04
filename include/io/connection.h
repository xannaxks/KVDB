#pragma once

#include "status.h"
#include <vector>
#include <byte>

class Connection
{	
public:
	Connection(int fd);

	void on_readable();
	void on_writable();

private:
	int fd_;

	std::vector<std::byte> input_buffer_;
	std::vector<std::byte> output_buffer_;
};