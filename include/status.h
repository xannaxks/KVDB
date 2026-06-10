#pragma once

#include <cstdint>
#include <string>
#include <utility>

enum class StatusCode : std::uint8_t {
    Ok = 0,

    // Compaction
    OverlappingKeys,

    // Generic
    InvalidArgument,
    FailedPrecondition,
    NotFound,
    AlreadyExists,
    NotSupported,
    InternalError,
    InvariantViolation,
    Duplicate,

    // Memory
    OutOfMemory,
    AllocationFailed,
    InvalidAlignment,
    AllocationTooLarge,
    BufferTooSmall,
    NullPointer,
    BadAlloc,

    // File helpers
    InvalidOffset,
    InvalidReadSize,
    SizeExceedsBlockSize,
    SizeExceedsBlockBoundary,

    // IO / syscalls
    IOError,
    OpenFailed,
    ReadFailed,
    WriteFailed,
    SyncFailed,
    CloseFailed,
    RenameFailed,
    DirectorySyncFailed,
    GetPositionFailed,
    GetSizeFailed,
    PermissionDenied,

    // File format
    Corruption,
    BadMagic,
    UnsupportedVersion,
    UnsupportedBlockSize,
    ChecksumMismatch,
    InvalidHeader,
    InvalidFooter,
    InvalidBlockType,
    InvalidBlockAlignment,
    InvalidSectionOffset,
    InvalidSectionSize,
    InvalidPayloadSize,
    OffsetOutOfRange,
    OffsetOverlap,
    UnexpectedEOF,
};
struct Status {
    Status() = default;
    Status(const Status&) = default;
    Status(Status&&) noexcept = default;

    Status(StatusCode code, std::string message = "")
        : code(code), message(std::move(message)) {
    }

    Status(StatusCode code, const char* message)
        : code(code), message(message) {
    }

    StatusCode code = StatusCode::Ok;
    std::string message;

    static Status ok();
    bool is_ok() const;

    Status& operator=(const Status&) = default;
    Status& operator=(Status&&) noexcept = default;
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

#include <system_error>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#endif

Status syscall_error(StatusCode code, std::string op);