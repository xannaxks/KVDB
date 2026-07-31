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
    const DWORD err = ::GetLastError(); // getting last error

    return Status
    {
        code,
        op + " failed: GetLastError=" + std::to_string(err) + " (" + std::system_category().message(err) + ")" 
        // returning message provided, last errors code and interpretation of last errors code
    };
#else
    const int err = errno; // getting last error

    return Status
    {
        code,
        op + " failed: errno=" + std::to_string(err) + " (" + std::generic_category().message(err) + ")"
        // returning message provided, last errors code and interpretation of last errors code
    };
#endif
}