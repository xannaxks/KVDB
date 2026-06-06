#include "file.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <cassert>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#else

#include <cerrno> // provides errno
#include <fcntl.h> // provides open flags like O_RDONLY, O_WRONLY, O_CREAT
#include <sys/stat.h> // provides file permissions constants
#include <unistd.h> // provides read/write/pread/fsync/close/rename

#endif

Status ReadableFile::read_exact_at(
	std::uint64_t offset,
	void* buffer,
	std::size_t size,
	std::uint64_t& track_offset
)
{
	std::size_t total_read = 0;

	char* out = static_cast<char*>(buffer);

	while (total_read < size)
	{
		std::size_t bytes_read = 0;

		const Status read_result = read_at(
			offset + total_read,
			out + total_read,
			size - total_read,
			bytes_read
		);

		if (!read_result.is_ok()) // os read failed
			return read_result;

		if (bytes_read == 0) // eof before reading enough bytes
			return Status{
				StatusCode::UnexpectedEOF,
				"Unexpected end of file during read of " + std::to_string(size) + " bytes on " + this->path.string()
			};

		total_read += bytes_read;

	}

	track_offset = offset + total_read;
	return Status::ok();
}

#ifdef _WIN32

class WindowsReadableFile final : public ReadableFile
{
private:
	HANDLE handle_ = INVALID_HANDLE_VALUE; // Windows file handle

public:
	explicit WindowsReadableFile(const std::filesystem::path& path)
	{
		handle_ = CreateFileW(
			path.c_str(),
			GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, // allowing other processes to access it
			nullptr, // Default securiyt attributes
			OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL,
			nullptr
		);

		if (handle_ == INVALID_HANDLE_VALUE) // file open failure
			throw std::runtime_error("CreateFileW for reading failed");
	}

	~WindowsReadableFile() override
	{
		if (handle_ != INVALID_HANDLE_VALUE)
			CloseHandle(handle_); // Close OS handle
	}

	Status read_at(
		std::uint64_t offset,
		void* buffer,
		std::size_t size,
		std::size_t& bytes_read
	) override
	{
		bytes_read = 0;

		char* out = reinterpret_cast<char*>(buffer);

		std::size_t remaining = size;

		while (remaining > 0)
		{
			const DWORD chunk = static_cast<DWORD>(
				std::min<std::size_t>(remaining, static_cast<std::size_t>(MAXDWORD)) // reading maxium possible sized chunk not larger than dword max
			);

			OVERLAPPED ov{};

			const std::uint64_t current = offset + bytes_read;

			ov.Offset = static_cast<DWORD>(current & 0xFFFFFFFFull);
			ov.OffsetHigh = static_cast<DWORD>((current >> 32) & 0xFFFFFFFFull);

			DWORD got = 0;

			const BOOL ok = ReadFile(
				handle_,
				out + bytes_read,
				chunk,
				&got,
				&ov
			);

			if (!ok)
				return syscall_error(StatusCode::ReadFailed, "read_at");

			if (got == 0)
				return Status::ok();

			bytes_read += got;
			remaining -= got;
		}

		return Status::ok();
	}

	Status close() override
	{
		if (handle_ == INVALID_HANDLE_VALUE)
			return Status::ok();

		const BOOL ok = CloseHandle(handle_);
		handle_ = INVALID_HANDLE_VALUE;
		return ok != 0 ? Status::ok() : syscall_error(StatusCode::CloseFailed, "close"); // used to convert windows BOOL to bool
	}

	Status get_file_size(std::uint64_t& size_out) override
	{
		LARGE_INTEGER li{};
		
		if (!::GetFileSizeEx(this->handle_, &li))
			return syscall_error(StatusCode::GetSizeFailed, "get_file_size");
		
		if (li.QuadPart < 0)
			return syscall_error(StatusCode::GetSizeFailed, "get_file_size");
		
		size_out = static_cast<std::uint64_t>(li.QuadPart);
		return Status::ok();
	}
};

class WindowsWritableFile final : public WritableFile // windows implementaion of Writable files
{
private:
	HANDLE handle_ = INVALID_HANDLE_VALUE;

public:
	explicit WindowsWritableFile(const std::filesystem::path& path)
	{
		handle_ = CreateFileW(
			path.c_str(),
			GENERIC_WRITE,
			0,
			nullptr,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr
		);

		if (handle_ == INVALID_HANDLE_VALUE)
			throw std::runtime_error("CreatFileW for writing failed");
	}

	~WindowsWritableFile() override
	{
		if (handle_ != INVALID_HANDLE_VALUE)
		{
			CloseHandle(handle_);
		}
	}

	Result<std::uint64_t> current_position() override
	{
		LARGE_INTEGER zero{};
		LARGE_INTEGER current{};

		const BOOL ok = SetFilePointerEx(
			handle_, zero, &current, FILE_CURRENT
		);

		if (ok == 0)
			return Result<std::uint64_t>::fail(syscall_error(StatusCode::GetPositionFailed, "current_position"));

		return Result<std::uint64_t>::ok(static_cast<std::uint64_t>(current.QuadPart));
	}

	Status append(
		const void* data,
		std::size_t size, 
		std::uint64_t& track_offset
	) override
	{
		Result<uint64_t> current_position_result = this->current_position();
		if(!current_position_result.is_ok())
			return current_position_result.status;
		assert(track_offset == current_position_result.value);

		const char* ptr = static_cast<const char*>(data);
		std::size_t remaining = size;

		while (remaining > 0)
		{
			const DWORD chunk = static_cast<DWORD>(
				std::min<std::size_t>(remaining, static_cast<std::size_t>(MAXDWORD))
			);

			DWORD written = 0;

			const BOOL ok = WriteFile(
				handle_,
				ptr,
				chunk,
				&written,
				nullptr
			);

			if (!ok) return syscall_error(StatusCode::WriteFailed, "append");
			if (written == 0) return syscall_error(StatusCode::WriteFailed, "append"); //os failure, didn't write anything

			track_offset += written;
			ptr += written;
			remaining -= written;

			Result<std::uint64_t> current_position_result = this->current_position();
			if(!current_position_result.is_ok())
				return current_position_result.status;
			assert(current_position_result.value == track_offset);
		}

		return Status::ok();
	}

	Status sync() override
	{
		if (handle_ == INVALID_HANDLE_VALUE)
			return syscall_error(StatusCode::SyncFailed, "sync");

		BOOL ok = FlushFileBuffers(handle_);
		if (ok == 0)
			return syscall_error(StatusCode::SyncFailed, "sync");

		return Status::ok();
	}

	Status close() override
	{
		if (handle_ == INVALID_HANDLE_VALUE)
			return Status::ok();

		const BOOL ok = CloseHandle(handle_);
		handle_ = INVALID_HANDLE_VALUE;

		return ok != 0 ? Status::ok() : syscall_error(StatusCode::CloseFailed, "close");
	}

	Status get_file_size(std::uint64_t& size_out) override
	{
		LARGE_INTEGER li{};

		if (!::GetFileSizeEx(this->handle_, &li))
			return syscall_error(StatusCode::GetSizeFailed, "get_file_size");

		if (li.QuadPart < 0)
			return syscall_error(StatusCode::GetSizeFailed, "get_file_size");

		size_out = static_cast<std::uint64_t>(li.QuadPart);
		return Status::ok();
	}

	Status durable_rename(const std::filesystem::path& to, bool replace_existing) override
	{
		DWORD flags = MOVEFILE_WRITE_THROUGH; // asking win to flush rename operation before returning

		if (replace_existing)
			flags |= MOVEFILE_REPLACE_EXISTING;

		BOOL ok = MoveFileExW(this->path.c_str(), to.c_str(), flags);
		if (ok == 0)
			return syscall_error(StatusCode::RenameFailed, "durable_rename");

		return Status::ok();
	}

	std::filesystem::path parent_directory() override
	{
		return this->path.parent_path();
	}

	Status sync_parent_directory()
	{
		const std::filesystem::path directory_path = this->parent_directory();
		HANDLE handle = CreateFileW(
			directory_path.c_str(),
			FILE_LIST_DIRECTORY, // access right for openning directory
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, // allowing sharing
			nullptr, // default security
			OPEN_EXISTING, // directory must exist
			FILE_FLAG_BACKUP_SEMANTICS, // required to open a directory with CreateFileW
			nullptr // no template file
		);

		if (handle == INVALID_HANDLE_VALUE)
			return syscall_error(StatusCode::DirectorySyncFailed, "sync_directory");

		BOOL ok = FlushFileBuffers(handle);

		if (ok == 0)
			return syscall_error(StatusCode::DirectorySyncFailed, "sync_directory");

		return this->close();

		return ok != 0 ? Status::ok() : syscall_error(StatusCode::CloseFailed, "sync_directory");
	}
};

#else

#include <limits>

class PosixReadableFile final : public ReadableFile
{
private:
	int fd_ = -1;

public:
	explicit PosixReadableFile(const std::filesystem::path& path)
	{
		this->path = path;

		fd_ = ::open(path.c_str(), O_RDONLY);

		if (fd_ == -1)
			throw std::runtime_error("open for reading failed");
	}

	~PosixReadableFile() override
	{
		if (fd_ != -1)
			::close(fd_);
	}

	Status read_at(
		std::uint64_t offset,
		void* buffer,
		std::size_t size,
		std::size_t& bytes_read
	) override
	{
		bytes_read = 0;

		if (fd_ == -1)
			return syscall_error(StatusCode::ReadFailed, "read_at");

		char* out = reinterpret_cast<char*>(buffer);

		while (bytes_read < size)
		{
			const std::size_t remaining = size - bytes_read;

			const std::size_t chunk = std::min<std::size_t>(
				remaining,
				static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())
			);

			const std::uint64_t current_offset = offset + bytes_read;

			if (current_offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
				return Status{
					StatusCode::ReadFailed,
					"read_at offset does not fit into off_t for file " + this->path.string()
			};

			const ssize_t got = ::pread(
				fd_,
				out + bytes_read,
				chunk,
				static_cast<off_t>(current_offset)
			);

			if (got < 0)
			{
				if (errno == EINTR)
					continue;

				return syscall_error(StatusCode::ReadFailed, "read_at");
			}

			if (got == 0)
				return Status::ok();

			bytes_read += static_cast<std::size_t>(got);
		}

		return Status::ok();
	}

	Status close() override
	{
		if (fd_ == -1)
			return Status::ok();

		const int result = ::close(fd_);
		fd_ = -1;

		return result == 0
			? Status::ok()
			: syscall_error(StatusCode::CloseFailed, "close");
	}

	Status get_file_size(std::uint64_t& size_out) override
	{
		if (fd_ == -1)
			return syscall_error(StatusCode::GetSizeFailed, "get_file_size");

		struct stat st {};

		if (::fstat(fd_, &st) != 0)
			return syscall_error(StatusCode::GetSizeFailed, "get_file_size");

		if (st.st_size < 0)
			return Status{
				StatusCode::GetSizeFailed,
				"file size is negative for file " + this->path.string()
		};

		size_out = static_cast<std::uint64_t>(st.st_size);
		return Status::ok();
	}
};

class PosixWritableFile final : public WritableFile
{
private:
	int fd_ = -1;

public:
	explicit PosixWritableFile(const std::filesystem::path& path)
	{
		this->path = path;

		fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

		if (fd_ == -1)
			throw std::runtime_error("open for writable file failed");
	}

	~PosixWritableFile() override
	{
		if (fd_ != -1)
			::close(fd_);
	}

	Result<std::uint64_t> current_position() override
	{
		if (fd_ == -1)
			return Result<std::uint64_t>::fail(
				syscall_error(StatusCode::GetPositionFailed, "current_position")
			);

		const off_t pos = ::lseek(fd_, 0, SEEK_CUR);

		if (pos == static_cast<off_t>(-1))
			return Result<std::uint64_t>::fail(
				syscall_error(StatusCode::GetPositionFailed, "current_position")
			);

		if (pos < 0)
			return Result<std::uint64_t>::fail(Status{
				StatusCode::GetPositionFailed,
				"current file position is negative for file " + this->path.string()
				});

		return Result<std::uint64_t>::ok(static_cast<std::uint64_t>(pos));
	}

	Status append(
		const void* data,
		std::size_t size,
		std::uint64_t& track_offset
	) override
	{
		Result<std::uint64_t> current_position_result = this->current_position();

		if (!current_position_result.is_ok())
			return current_position_result.status;

		assert(track_offset == current_position_result.value);

		const char* ptr = static_cast<const char*>(data);
		std::size_t remaining = size;

		while (remaining > 0)
		{
			const std::size_t chunk = std::min<std::size_t>(
				remaining,
				static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())
			);

			const ssize_t written = ::write(fd_, ptr, chunk);

			if (written < 0)
			{
				if (errno == EINTR)
					continue;

				return syscall_error(StatusCode::WriteFailed, "append");
			}

			if (written == 0)
				return Status{
					StatusCode::WriteFailed,
					"write returned 0 bytes for file " + this->path.string()
			};

			ptr += written;
			track_offset += static_cast<std::uint64_t>(written);
			remaining -= static_cast<std::size_t>(written);

			current_position_result = this->current_position();

			if (!current_position_result.is_ok())
				return current_position_result.status;

			assert(current_position_result.value == track_offset);
		}

		return Status::ok();
	}

	Status sync() override
	{
		if (fd_ == -1)
			return syscall_error(StatusCode::SyncFailed, "sync");

		while (true)
		{
			const int result = ::fsync(fd_);

			if (result == 0)
				return Status::ok();

			if (errno == EINTR)
				continue;

			return syscall_error(StatusCode::SyncFailed, "sync");
		}
	}

	Status close() override
	{
		if (fd_ == -1)
			return Status::ok();

		const int result = ::close(fd_);
		fd_ = -1;

		return result == 0
			? Status::ok()
			: syscall_error(StatusCode::CloseFailed, "close");
	}

	Status get_file_size(std::uint64_t& size_out) override
	{
		if (fd_ == -1)
			return syscall_error(StatusCode::GetSizeFailed, "get_file_size");

		struct stat st {};

		if (::fstat(fd_, &st) != 0)
			return syscall_error(StatusCode::GetSizeFailed, "get_file_size");

		if (st.st_size < 0)
			return Status{
				StatusCode::GetSizeFailed,
				"file size is negative for file " + this->path.string()
		};

		size_out = static_cast<std::uint64_t>(st.st_size);
		return Status::ok();
	}

	Status durable_rename(
		const std::filesystem::path& to,
		bool replace_existing
	) override
	{
		if (!replace_existing)
		{
			struct stat st {};

			if (::stat(to.c_str(), &st) == 0)
			{
				return Status{
					StatusCode::AlreadyExists,
					"rename destination already exists: " + to.string()
				};
			}

			if (errno != ENOENT)
				return syscall_error(StatusCode::RenameFailed, "durable_rename");
		}

		if (::rename(this->path.c_str(), to.c_str()) != 0)
			return syscall_error(StatusCode::RenameFailed, "durable_rename");

		this->path = to;
		return Status::ok();
	}

	std::filesystem::path parent_directory() override
	{
		const std::filesystem::path parent = this->path.parent_path();

		if (parent.empty())
			return ".";

		return parent;
	}

	Status sync_parent_directory() override
	{
		const std::filesystem::path directory_path = this->parent_directory();

#ifdef O_DIRECTORY
		const int flags = O_RDONLY | O_DIRECTORY;
#else
		const int flags = O_RDONLY;
#endif

		const int dir_fd = ::open(directory_path.c_str(), flags);

		if (dir_fd == -1)
			return syscall_error(StatusCode::DirectorySyncFailed, "sync_parent_directory");

		Status sync_status = Status::ok();

		while (true)
		{
			const int result = ::fsync(dir_fd);

			if (result == 0)
			{
				sync_status = Status::ok();
				break;
			}

			if (errno == EINTR)
				continue;

			sync_status = syscall_error(
				StatusCode::DirectorySyncFailed,
				"sync_parent_directory"
			);
			break;
		}

		const int close_result = ::close(dir_fd);

		if (close_result != 0 && sync_status.is_ok())
			return syscall_error(StatusCode::CloseFailed, "sync_parent_directory");

		return sync_status;
	}
};

#endif

Result<std::unique_ptr<ReadableFile>> open_readable_file(const std::filesystem::path& path)
{
	try
	{
#ifdef _WIN32
		return Result<std::unique_ptr<ReadableFile>>::ok(
			std::make_unique<WindowsReadableFile>(path)
		);
#else
		return Result<std::unique_ptr<ReadableFile>>::ok(
			std::make_unique<PosixReadableFile>(path)
		);
#endif
	}
	catch (const std::bad_alloc&)
	{
		return Result<std::unique_ptr<ReadableFile>>::fail(
			Status
			{
				StatusCode::BadAlloc,
				"allocation failed while opening readable file: " + path.string()
			}
		);
	}
	catch (const std::exception& e)
	{
		return Result<std::unique_ptr<ReadableFile>>::fail(
			Status
			{
				StatusCode::OpenFailed,
				"open readable file failed for " + path.string() + ": " + e.what()
			}
		);
	}
}

Result<std::unique_ptr<WritableFile>> open_writable_file(const std::filesystem::path& path)
{
	try
	{
#ifdef _WIN32
		return Result<std::unique_ptr<WritableFile>>::ok(
			std::make_unique<WindowsWritableFile>(path)
		);
#else
		return Result<std::unique_ptr<WritableFile>>::ok(
			std::make_unique<PosixWritableFile>(path)
		);
#endif
	}
	catch (const std::bad_alloc&)
	{
		return Result<std::unique_ptr<WritableFile>>::fail(
			Status{
				StatusCode::BadAlloc,
				"allocation failed while opening writable file: " + path.string()
			}
		);
	}
	catch (const std::exception& e)
	{
		return Result<std::unique_ptr<WritableFile>>::fail(
			Status{
				StatusCode::OpenFailed,
				"open writable file failed for " + path.string() + ": " + e.what()
			}
		);
	}
}