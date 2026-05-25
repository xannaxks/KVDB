#include "status.h"

Status Status::ok()
{
	return {};
}

bool Status::is_ok() const
{
	return this->code == StatusCode::Ok;
}