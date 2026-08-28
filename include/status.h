/**
* @file status.h
* @brief Provides classes, types and utilities for error handling.
*
* Instead of terminating execution when an operation fails, these entities
* allow functions to return objects containing an appropriate error code and
* descriptive message. This enables callers to handle failures explicitly.
*
* @note These entities are not currently thread-safe.
*/

#pragma once
#define NOMINMAX // disabling Windows min and max functions, to avoid collission with STL

#include <cstdint>
#include <string>
#include <utility>

/**
* @brief Error's type indication.
*/
enum class StatusCode : std::uint8_t {
    
    Ok = 0, ///< success
    UseAfterClose, ///< resource was closed, yet attempt to perform operations over it were tried.
    InvalidState, ///< invalid state of entity. might include
    DataTypeOverflow, ///< data type overflow
    // Compaction
    OverlappingKeys, ///< no overlapping keys allowed on L1+ levels.

    // Generic
    BadAccess, ///< permissions denied, use after close
    Underflow, ///< Arithmetic or cursor underflow.
    InvalidArgument, ///< invalid argument was provided
    FailedPrecondition, ///< failed to satify preconditions
    NotFound, ///< not found
    AlreadyExists, ///< typical scenario when file already exists, but attempt to recreate it was performed.
    NotSupported, ///< Not suppported, might refer to invalid 
    InternalError, ///< Unexpected invariant or implementation failure.
    InvariantViolation, ///< Violates entities invariants.
    Duplicate, ///< duplicate. in some cases, like L1+ level tables, might mean corruption.

    // Memory
    OutOfMemory, ///< out of aviable memory, usuallly fatal.
    AllocationFailed, ///< allocation of memory failed, might be fatal.
    InvalidAlignment, ///< invalid entity alignment was provided, maximum allowed alignment is std::max_align_t
    AllocationTooLarge, ///< Requested allocation exceeds supported limits.
    BufferTooSmall, ///< provided buffer is too small, might lead to overflow of buffer
    NullPointer, ///< reference to null pointer.
    BadAlloc, ///< Bad allocation

    // File helpers
    InvalidOffset, ///< invalid offset
    InvalidReadSize, ///< invalid amount of bytes requested to read
    SizeExceedsBlockSize, ///< size of entity overflow size of block. @note Consider records fragmentation to avoid it.
    SizeExceedsBlockBoundary, ///< requested amount of bytes to process move cursor (manual has priority over internal) out of current block's boundaries.

    // IO / syscalls
    IOError, ///< io error
    OpenFailed, ///< failed to open, might be denied access
    ReadFailed, ///< read failed
    WriteFailed, ///< write failed
    SyncFailed, ///< failed to flush data to persistent storage
    CloseFailed, ///< failed to close resource
    RenameFailed, ///< failed to rename resource
    DirectorySyncFailed, ///< failed to flush directory changes to persistent storage
    GetPositionFailed, ///< failed to get position of files internal cursor.
    GetSizeFailed, ///< failed to get size of file.
    PermissionDenied, ///< permission denied.
    SeekFailed, ///< failed to move positions of files internal cursor.

    // File format
    Corruption, ///< Corruption, not necessarily fatal.
    BadMagic, ///< Bad magic number, fatal.
    UnsupportedVersion, ///< Unsupported version, not necessarily fatal.
    UnsupportedBlockSize, ///< Unsupported block size
    ChecksumMismatch, ///< Checksum mismatch
    InvalidHeader, ///< invalid header of sstable section.
    InvalidFooter, ///< invalid footer section
    InvalidBlockType, ///< invalid block/section type of sstable.
    InvalidBlockAlignment, ///< default data block must be alignment to block boundaries.
    InvalidSectionOffset, ///< invalid offset of sstable section (data section, index section etc.) 
    InvalidSectionSize, ///< invalid size of sstable section.
    InvalidPayloadSize, ///< invalid payload size of sstable section.
    OffsetOutOfRange, ///< offset points over eof.
    OffsetOverlap, ///< offset overlap
    UnexpectedEOF, ///< unexpected end of file, not necessarily fatal.
    BadFileDescriptor, ///< bad file descriptor, might mean that file alrady closed.

	InsertionFailed, ///< insertion failed, might be due to memory allocation failure or other reasons.
};

/**
* @brief Indicates status of operations final result.
* 
* Consider using with void functions.
* In case of functions that return value, consider using Result<T>.
*/
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

    StatusCode code = StatusCode::Ok; ///< default status code indicates success.
    std::string message; ///< message providing detailed information.

    /**
    * @brief Returns success statuscode and empty message.
    * 
    * @return Status{StatusCode::Ok}
    */
    static Status ok();

    /**
    * @brief Returns status indicating invalid argument with message providing detailed information.
    * 
    * @param[in] message message with detailed information.
    * 
    * @return Status{StatusCode::InvalidArgument, std::string}
    */
    static Status invalid_argument(std::string message)
    {
        return Status{ StatusCode::InvalidArgument, std::move(message) };
    }

    /**
    * @brief Checks whether StatusCode is StatusCode::Ok.
    * 
    * @retval true in case if StatusCode == StatusCode::Ok
    * @retval false otherwise
    */
    bool is_ok() const;

    Status& operator=(const Status&) = default;
    Status& operator=(Status&&) noexcept = default;
};

/**
* @brief Returns status of functions final result, with result itself.
* 
* Consider using with functions that have return value. T indicates the
* data type of return value. 
* Ex: Result<bool> test()
*        return Result<bool>::ok(true);
* 
* @note Consider using std::optional<T> to avoid construction of empty object.
*/
template <typename T>
struct [[nodiscard]] Result
{
	T value; ///< data type of functions return value.
	Status status; ///< status indicating status code and message.

	static Result ok(T v)
	{
		return Result{ std::move(v), Status::ok() };
	}

    /**
    * @brief Use this in case of failure, return empty object
    * 
    * @param[in] s Status object, indicating final result of function execution.
    * 
    * @note This statis method constructs empty object, which might be expensive.
    * @note Consider using std::optional<T> to avoid construction of empty object.
    */
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

/**
* @brief Returns status indicating system call error/failure.
* 
* @param[in] code StatusCode indicating system call error or any other.
* @param[in] op Optional message prefix. It should not contain last syscall error codes interpretation.
* 
* @return Status Indicating status of syscall error, with message containg error code and its interpretation.
*/
Status syscall_error(StatusCode code, std::string op);
