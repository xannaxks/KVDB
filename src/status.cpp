#include "status.h"

Status Status::ok()
{
	return {};
}

bool Status::is_ok() const
{
	return this->code == StatusCode::Ok;
}

Status syscall_error(StatusCode code, std::string op)
{
#ifdef _WIN32
    const DWORD err = ::GetLastError();

    return Status
    {
        code,
        op + " failed: GetLastError=" + std::to_string(err) + " (" + std::system_category().message(err) + ")"
    };
#else
    const int err = errno;

    return Status
    {
        code,
        op + " failed: errno=" + std::to_string(err) + " (" + std::generic_category().message(err) + ")"
    };
#endif
}