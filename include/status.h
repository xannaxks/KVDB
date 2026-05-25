#include <cstdint>
#include <string>

enum class StatusCode : uint8_t {
    Ok,

    // Generic
    InvalidArgument,
    NotFound,
    AlreadyExists,
    NotSupported,
    InvariantViolation,

    // Memory / allocation
    OutOfMemory,
    AllocationFailed,
    InvalidAlignment,
    AllocationTooLarge,
    BufferTooSmall,
    NullPointer,

    // IO / OS
    IOError,
    OpenFailed,
    ReadFailed,
    WriteFailed,
    SyncFailed,
    RenameFailed,
    DirectorySyncFailed,

    // File format / corruption
    Corruption,
    BadMagic,
    UnsupportedVersion,
    ChecksumMismatch,
    InvalidBlockAlignment,
    InvalidBlockType,
    InvalidSize,
    UnexpectedEOF,

    BadAlloc,
};

struct Status
{
	StatusCode code = StatusCode::Ok;
	std::string message;

	static Status ok();

	bool is_ok() const;
};

template <typename T>
struct [[nodiscard]] Result
{
	T value;
	Status status;

	static Result ok(T v)
	{
		return Result{ std::move(v), Status::ok() };
	}

	static Result fail(Status s)
	{
		return Result{ T{}, std::move(s) };
	}

	bool is_ok() const
	{
		return this->status.is_ok();
	}
};