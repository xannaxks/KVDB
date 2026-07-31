/**
 * @file file.h
 * @brief Provides platform-specific wrappers around OS file and directory
 *        system calls.
 *
 * Supports file reading and writing, synchronization, directory operations,
 * and atomic file renaming.
 * 
 * @note Entities are not concurrency safe for now.
 * @note Entities don't guarentee atomicity and durability. 
 * 
 * @todo Ensure atomicity and durability.
 * @todo Add concurrency safety.
 */
#pragma once

#define NOMINMAX // Disable Windows min() and max() functions, in order to prevent collissions with STL.
#define WIN32_LEAN_AND_MEAN // Exclude rarely used Windows APIs

#include "status.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <filesystem>

/**
 * @brief Abstract interface for readable files.
 *
 * Defines the operations that platform-specific implementations, such as
 * Windows and POSIX readable files, must provide.
 *
 * Implementations must override operations including:
 * - read_at()
 * - close()
 * - get_file_size()
 *
 * @note This class is not thread-safe.
 */
class ReadableFile
{
public:
	std::filesystem::path path; ///< Path of file being read.

	/**
	 * @brief Virtual destructor for polymorphic destruction.
	 */
	virtual ~ReadableFile() = default;

	/**
	 * @brief Reads up to the requested number of bytes from the givern offset.
	 * 
	 * This method should be implemented by derived classes using appropriate 
	 * system calls, such as ReadFile() on Windows or ::pread() on POSIX systems.
	 * 
	 * @param[in] offset File offset at which reading begins.
	 * @param[out] buffer Destination buffer.
	 * @param[in] size Number of bytes to read.
	 * @param[out] bytes_read Number of bytes read.
	 * 
	 * @return Status::ok() in case of succesfull read of required amount of bytes.
	 * 
	 * @retval Status{DataTypeOverflow} in case of off_t overflow, usually if @p offset cannot be represented 
	 * by the platform-specific offset type.
	 * @retval Status{ReadFailed} In case of failed read system call or attempt on closed file, including partial reads.
	 * 
	 * @note Partial reads can occur, its normal behavior. Consider retrying or using read_exact_at() method.
	 */	
	virtual Status read_at(
		std::uint64_t offset,
		void* buffer,
		std::size_t size,
		std::size_t& bytes_read
	) = 0;

	/**
	 * @brief Reads exactly the requested number of bytes from the given offset.
	 *
	 * This method repeatedly calls the virtual read_at() operation through the
	 * ReadableFile interface until the destination buffer is completely filled
	 * or an error or unexpected end-of-file condition occurs.
	 *
	 * Derived classes must implement read_at(); they normally do not need to
	 * override this method.
	 *
	 * @param offset File offset at which reading begins.
	 * @param buffer Destination buffer.
	 * @param size Number of bytes to read.
	 * @param track_offset Manual cursor/offset tracker.
	 *
	 * @return Status indicating success or the encountered error.
	 */
	Status read_exact_at(
		std::uint64_t offset,
		void* buffer,
		std::size_t size,
		std::uint64_t& track_offset
	);

	/**
	 * @brief Closes the underlying file.
	 *
	 * Derived classes must implement this method using the appropriate
	 * platform-specific system call, such as CloseHandle() on Windows or
	 * close() on POSIX systems.
	 *
	 * @return Status::ok() on success; otherwise, a status describing the system-call failure.
	 */
	virtual Status close() = 0;

	/**
	 * @brief Retrieves the size of the underlying file.
	 *
	 * Derived classes must implement this method using the appropriate
	 * platform-specific system call, such as GetFileSizeEx() on Windows or
	 * ::fstat() on POSIX systems.
	 *
	 * @param[out] size_out Receives the file size in bytes.
	 *
	 * @return Status::ok() on success; otherwise, a status describing the failure.
	 *
	 * @retval Status{GetSizeFailed} If the file-size query fails or the file is closed.
	 */
	virtual Status get_file_size(std::uint64_t& size_out) = 0;
	
#ifdef _WIN32
	/**
	 * @brief Gets the native Windows handle of the underlying file.
	 *
	 * Derived WindowsReadableFile classes must implement this method by returning
	 * the underlying file handle.
	 *
	 * @return The underlying Windows handle represented as a const void pointer.
	 *
	 * @warning Returning a native handle as const void* loses type safety and is
	 *          generally not recommended. Prefer returning HANDLE directly when
	 *          possible.
	 *
	 * @warning The returned handle is owned by this object. The caller must not
	 *          close it or retain it after the file object is destroyed or closed.
	 */
	virtual const void* get_descriptor() const = 0;
#else
	/**
	 * @brief Gets the native POSIX file descriptor of the underlying file.
	 *
	 * Derived PosixReadableFile classes must implement this method by returning a
	 * reference to the underlying file descriptor.
	 *
	 * @return A const reference to the underlying POSIX file descriptor.
	 *
	 * @warning Returning the descriptor by reference exposes internal object
	 *          storage and may produce a dangling reference after the file object
	 *          is destroyed. Returning int by value is generally safer and
	 *          recommended.
	 *
	 * @warning The descriptor is owned by this object. The caller must not close
	 *          it or retain the reference after the file object is destroyed or
	 *          closed.
	 */
	virtual const int& get_descriptor() const = 0;
#endif
};

/**
 * @brief Abstract interface for writeable files.
 *
 * Defines the operations that platform-specific implementations, such as
 * Windows and POSIX writable files, must provide.
 *
 * Implementations must override operations including:
 * - sync()
 * - append()
 * - close()
 * - get_file_size()
 * - current_position()
 * - durable_rename()
 * - sync_parent_directory()
 * - parent_directory()
 * - seek_to_end()
 * 
 * @note LSM-based databases generally use ReadableFile objects primarily for
 * read operations, which is why WritableFile provides a larger set of methods.
 * 
 * @note This class is not thread-safe.
 */
class WritableFile
{
public:
	std::filesystem::path path; ///< Path of file being written.

	/**
	 * @brief Virtual destructor for polymorphic destruction.
	 */
	virtual ~WritableFile() = default;

	/**
	 * @brief Appends data to the writable file, based on internal cursors position.
	 * 
	 * Derived classes must implement this methods using appropriate system calls
	 * such as WriteFile on Windows or ::write on POSIX systems.
	 * 
	 * @param[in] data Pointer to data bytes.
	 * @param[in] size Amount of bytes to be written.
	 * @param track_offset Manual cursor/offset tracker.
	 * 
	 * @return Status::ok() in case of success, otherwise Status with appropriate StatusCode
	 * 
	 * @retval Status{WriteFailed} In case of failed write system call or attempt on closed file,
	 * including partial writes.
	 * 
	 * @note Partial writes doesn't necessarily mean fatal error. Consider retrying or using helpers.
	 */
	virtual Status append(
		const void* data,
		std::size_t size, 
		std::uint64_t& track_offset
	) = 0;

	/**
	 * @brief Flushes buffered file data to stable storage.
	 * 
	 * Derived classes must implement this method using appropriate system calls
	 * such as FlushFileBuffers on Windows and ::fsync on POSIX systems.
	 * 
	 * @return Status::ok() in case of success, otherwise Status with describing the failure.
	 * 
	 * @retval Status{SyncFailed} If the underlying synchronization operation fails or the file is closed.
	 */
	virtual Status sync() = 0;

	/**
	 * @brief Closes the underlying file.
	 *
	 * Derived classes must implement this method using the appropriate
	 * platform-specific system call, such as CloseHandle() on Windows or
	 * close() on POSIX systems.
	 *
	 * @return Status::ok() on success; otherwise, a status describing the system-call failure.
	 */
	virtual Status close() = 0;

	/**
	* @brief Returns the current position of the internal file cursor.
	*
	* Derived classes must implement this method using the appropriate
	* platform-specific system call, such as SetFilePointerEx() on Windows or
	* ::lseek() on POSIX systems.
	*
	* @return A successful Result containing the current cursor position as a
	*         std::uint64_t; otherwise, a failed Result describing the error.
	*
	* @retval Result<std::uint64_t>::fail(GetPositionFailed) If the file is closed,
	*         the system call fails, or the returned position is invalid or cannot
	*         be represented as std::uint64_t.
	*/
	virtual Result<std::uint64_t> current_position() = 0;

	/**
	 * @brief Retrieves the size of the underlying file.
	 *
	 * Derived classes must implement this method using the appropriate
	 * platform-specific system call, such as GetFileSizeEx() on Windows or
	 * ::fstat() on POSIX systems.
	 *
	 * @param[out] size_out Receives the file size in bytes.
	 *
	 * @return Status::ok() on success; otherwise, a status describing the failure.
	 *
	 * @retval Status{GetSizeFailed} If the file-size query fails or the file is closed.
	 */
	virtual Status get_file_size(std::uint64_t& size_out) = 0;

	/**
	* @brief Renames the underlying file.
	*
	* Derived classes must implement this method using the appropriate
	* platform-specific operation, such as MoveFileExW() on Windows or
	* ::rename() on POSIX systems.
	*
	* @param[in] to Destination path containing the new file name.
	* @param[in] replace_existing Whether an existing destination file may be
	*             replaced.
	*
	* @return Status::ok() on success; otherwise, a status describing the failure.
	*
	* @todo Consider combining rename(), file synchronization, and parent-directory
	*       synchronization into a single durable replacement operation.
	*
	* @warning This method has not been verified for crash atomicity or durability.
	*
	* @note The rename operation does not by itself guarantee that the directory
	*       metadata has reached persistent storage.
	*
	* @note On POSIX systems, ::rename() provides atomic namespace replacement when
	*       its platform and filesystem requirements are satisfied. However, a
	*       successful return does not by itself make the rename durable across a
	*       system crash.
	*
	* @note On Windows, MoveFileExW() does not provide a general guarantee that the
	*       replacement is crash-atomic or durable on every filesystem and storage
	*       configuration.
	*
	* @details On POSIX systems, durable replacement normally requires synchronizing
	*          the newly written file before renaming it and synchronizing the
	*          affected parent directory after the rename. If the source and
	*          destination have different parent directories, both directories may
	*          need to be synchronized.
	*
	* @details On Windows, MOVEFILE_WRITE_THROUGH requests that MoveFileExW() wait
	*          until the move operation is completed. It should not be treated as a
	*          universal guarantee that all file and directory metadata will survive
	*          every possible system or storage failure.
	*/
	virtual Status durable_rename(const std::filesystem::path& to, bool replace_existing) = 0;

	/**
	* @brief Synchronization/flushing of parent directories metadata to persistent storage.
	* 
	* Derived classes must implement this method using appropriate platform-dependent system
	* calls such as ::fsync on POSIX sytsems. 
	* 
	* @note Unlike the POSIX implementation, Windows provides no documented
	*       per-directory equivalent of fsync(parent_directory_fd). Therefore,
	*       identical crash-durability guarantees are not claimed across platforms.
	* 
	* @return Status with operations result.
	* 
	* @retval Status::ok() in case of success.
	* @retval Status{DirectorySyncFailed} in case of system call failure, or attempt over closed file.
	* 
	* @warning This method has not been verified for crash atomicity or durability.
	*/
	virtual Status sync_parent_directory() = 0;

	/**
	* @brief Returns parent directory of underlying file.
	* 
	* Derived classes must implement this methods using filesystem library's entities.
	* 
	* @return std::filesystem::path of underlying file's parent.
	*/
	virtual std::filesystem::path parent_directory() = 0;

	/**
	* @brief Places underlying file's cursor at the end.
	* 
	* Derived classes must implement this method using appropriate platform-dependent system calls
	* such as SetFilePointerEx() on Windows or ::lseek() on POSIX systems.
	* 
	* @return Result<std::uint64_t> with appropriate status and position of internal cursor, which now points to eof,
	*		  in case of success.
	* 
	* @retval Result<std::uint64_t>::ok(std::uint64_t) in case of success, containing internal cursor's new position.
	* @retval Result<std::uint64_t>::fail(SeekFailed) in case of failure, with appropriate StatusCode
	*/
	virtual Result<std::uint64_t> seek_to_end() = 0;

#ifdef _WIN32
	/**
	 * @brief Gets the native Windows handle of the underlying file.
	 *
	 * Derived WindowsReadableFile classes must implement this method by returning
	 * the underlying file handle.
	 *
	 * @return The underlying Windows handle represented as a const void pointer.
	 *
	 * @warning Returning a native handle as const void* loses type safety and is
	 *          generally not recommended. Prefer returning HANDLE directly when
	 *          possible.
	 *
	 * @warning The returned handle is owned by this object. The caller must not
	 *          close it or retain it after the file object is destroyed or closed.
	 */
	virtual const void* get_descriptor() const = 0;
#else
	/**
	 * @brief Gets the native POSIX file descriptor of the underlying file.
	 *
	 * Derived PosixReadableFile classes must implement this method by returning a
	 * reference to the underlying file descriptor.
	 *
	 * @return A const reference to the underlying POSIX file descriptor.
	 *
	 * @warning Returning the descriptor by reference exposes internal object
	 *          storage and may produce a dangling reference after the file object
	 *          is destroyed. Returning int by value is generally safer and
	 *          recommended.
	 *
	 * @warning The descriptor is owned by this object. The caller must not close
	 *          it or retain the reference after the file object is destroyed or
	 *          closed.
	 */
	virtual const int& get_descriptor() const = 0;
#endif
};

/**
* @brief Opens appropriate readable file
* 
* Opens appropriate readable file based on platform. Returns result as pointer to 
* base class.
* 
* @param[in] path path to readable file.
* 
* @return Result<std::unique_ptr<ReadableFile>> with appropriate Status and value.
* 
* @retval Result<std::unique_ptr<ReadableFile>>::ok(std::unique_ptr<ReadableFile>)
*         in case of success, contains base class pointer to newly opened file.
* @retval Result<std::unique_ptr<ReadableFile>>::fail(BadAlloc) in case of allocation failure.
* @retval Result<std::unique_ptr<ReadableFile>>::fail(OpenFailed) in case of system call failure.
* 
*/
Result<std::unique_ptr<ReadableFile>> open_readable_file(
	const std::filesystem::path& path
);

/**
* @brief Opens appropriate writable file
*
* Opens appropriate writable file based on platform. Returns result as pointer to
* base class. Moves underlying files internal cursor to the end of file (for correct 
* behavior of WritableFile::append() method).
*
* @param[in] path path to writable file.
*
* @return Result<std::unique_ptr<WritableFile>> with appropriate Status and value.
*
* @retval Result<std::unique_ptr<WritableFile>>::ok(std::unique_ptr<WritableFile>)
*         in case of success, contains base class pointer to newly opened file.
* @retval Result<std::unique_ptr<WritableFile>>::fail(BadAlloc) in case of allocation failure.
* @retval Result<std::unique_ptr<WritableFile>>::fail(OpenFailed) in case of system call failure.
*
*/
Result<std::unique_ptr<WritableFile>> open_writable_file(
	const std::filesystem::path& path
);

//Result<std::unique_ptr<WritableFile>> open_writable_file(const std::filesystem::path& path);
//Result<std::unique_ptr<ReadableFile>> open_readable_file(const std::filesystem::path& path);

//#ifdef _WIN32
//
//class WindowsReadableFile : public ReadableFile;
//class WindowsWritableFile : public WritableFile;
//
//#else
//
//class PosixReadableFile : public ReadableFile;
//class PoxisWriteableFile : public WritableFile;
//
//#endif

