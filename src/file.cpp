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

bool ReadableFile::read_exact_at(
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

		const bool ok = read_at(
			offset + total_read,
			out + total_read,
			size - total_read,
			bytes_read
		);

		if (!ok) // os read failed
			return false;

		if (bytes_read == 0) // eof before reading enough bytes
			return false;

		total_read += bytes_read;

	}

	track_offset = offset + total_read;
	return true;
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

	bool read_at(
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
				return false;

			if (got == 0)
				return true;

			bytes_read += got;
			remaining -= got;
		}

		return true;
	}

	bool close() override
	{
		if (handle_ == INVALID_HANDLE_VALUE)
			return true;

		const BOOL ok = CloseHandle(handle_);
		handle_ = INVALID_HANDLE_VALUE;
		return ok != 0; // used to convert windows BOOL to bool
	}

	bool get_file_size(std::uint64_t& size_out) override
	{
		LARGE_INTEGER li{};
		
		if (!::GetFileSizeEx(this->handle_, &li))
			return false;
		
		if (li.QuadPart < 0)
			return false;
		
		size_out = static_cast<std::uint64_t>(li.QuadPart);
		return true;
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

	bool append(
		const void* data,
		std::size_t size, 
		std::uint64_t& track_offset
	) override
	{
		assert(track_offset == this->current_position());

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

			if (!ok) return false;
			if (written == 0) return false; //os failure, didn't write anything

			track_offset += written;
			ptr += written;
			remaining -= written;

			assert(this->current_position() == track_offset);
		}

		return true;
	}

	bool sync() override
	{
		if (handle_ == INVALID_HANDLE_VALUE)
			return false;

		return FlushFileBuffers(handle_) != 0;
	}

	bool close() override
	{
		if (handle_ == INVALID_HANDLE_VALUE)
			return true;

		const BOOL ok = CloseHandle(handle_);
		handle_ = INVALID_HANDLE_VALUE;

		return ok != 0;
	}

	std::uint64_t current_position() override
	{
		LARGE_INTEGER zero{};
		LARGE_INTEGER current{};

		const BOOL ok = SetFilePointerEx(
			handle_, zero, &current, FILE_CURRENT
		);

		if (!ok)
			throw std::runtime_error("SetFilePointerEx failed");

		return static_cast<std::uint64_t>(current.QuadPart);
	}

	bool get_file_size(std::uint64_t& size_out) override
	{
		LARGE_INTEGER li{};

		if (!::GetFileSizeEx(this->handle_, &li))
			return false;

		if (li.QuadPart < 0)
			return false;

		size_out = static_cast<std::uint64_t>(li.QuadPart);
		return true;
	}
};

bool durable_rename(const std::filesystem::path& from, const std::filesystem::path& to, bool replace_existing)
{
	DWORD flags = MOVEFILE_WRITE_THROUGH; // asking win to flush rename operation before returning

	if (replace_existing)
		flags |= MOVEFILE_REPLACE_EXISTING;

	return MoveFileExW(from.c_str(), to.c_str(), flags) != 0;
}

static std::filesystem::path parent_directory_of(const std::filesystem::path& path)
{
	return path.parent_path();
}

bool sync_directory(const std::filesystem::path& directory_path)
{
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
		return false;

	const BOOL ok = FlushFileBuffers(handle);

	CloseHandle(handle);

	return ok != 0;
}

bool sync_parent_directory(const std::filesystem::path& path)
{
	return sync_directory(parent_directory_of(path));
}

#else

class PosixReadableFile : public ReadableFile // POSIX implementation of ReadableFile
{
private:
	int fd_ = -1; // posix file descriptor

public:
	explicit PosixReadableFile(const NativePath& path)
	{
		fd_ = ::open(path.c_str(), O_RDONLY); // readonly

		if (fd_ == -1)
			throw std::runtime_error("open for reading failed");
	}

	~PosixReadableFile() override
	{
		if (fd_ != -1)
			::close(fd_);
	}

	bool read_at(
		std::uint64_t offset,
		void* buffer,
		std::size_t size,
		std::size_t& bytes_read
	) override
	{
		bytes_read = 0;
		
		char* out = reinterpret_cast<char*>(buffer);

		while (bytes_read < size)
		{
			const std::size_t remaining = size - bytes_read;

			const ssize_t got =
				::pread(fd_, out + bytes_read, remaining, static_cast<off_t>(offset + bytes_read));

			if (got < 0) // error occured
			{
				if (errno == EINTR) // interrupted by signal
					continue; // retry

				return false;
			}

			if (got == 0) // eof reached
				return true;

			bytes_read += static_cast<std::size_t>(got);
		}

		return true;
	}

	bool close() override
	{
		if (fd_ == -1)
			return true;

		const int result = ::close(fd_);
		fd_ = -1;

		return result == 0; // POSIX close returns 0 on success
	}

	bool get_file_size(std::uint64_t& size_out) override
	{
		struct stat st {};
			
		if (::fstat(this->fd, &st) != 0)
			return false;
		
		if (st.st_size() < 0)
			return false;
		
		size_out = static_cast<std::uint64_t>(st.st_size());
		return true;
	}
};

class PosixWritableFile final : public WritableFile
{
private:
	int fd_ = -1;

public:
	explicit PosixWritableFile(const NativePath& path)
	{
		fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644); // write only, create if wasn't, truncate if exists

		if (fd_ == -1)
			throw std::runtime_error("open for Writable file failed");
	}

	~PosixWritableFile() override
	{
		if (fd_ != -1)
			::close(fd_);
	}

	bool append(const void* data, std::size_t size, std::uint64_t& track_offset) override
	{
		assert(this->current_position() == track_offset);

		const char* ptr = static_cast<const char*>(data);
		std::size_t remaining = size;
		
		while (remaining > 0)
		{
			const ssize_t written = ::write(fd_, ptr, remaining);

			if (written < 0)
			{
				if (errno == EINTR) //interrupted by signal
					continue;
				return false;
			}

			if (written == 0)
				return false; // avoiding infinite loop

			ptr += written;
			track_offset += written;
			remaining -= static_cast<std::size_t>(written);
		
			assert(this->current_position() == track_offset);
		}

		return true;
	}

	bool sync() override
	{
		if (fd_ == -1) return false;
		while (true)
		{
			const int result = ::fsync(fd_);

			if (result == 0) return true; // sync success
			if (errno == EINTR) //signal interrupted
				continue; // try again

			return false;
		}
	}

	bool close() override
	{
		if (fd_ == -1)
			return true;

		const int result = ::close(fd_);//  close can report delayed writebackk errors
		fd_ = -1;

		return result == 0;
	}

	std::uint64_t current_position() override
	{
		off_t pos = ::lseek(this->fd_, 0, SEEK_CUR);

		if (pos == static_cast<off_t>(-1))
			throw std::runtime_error("lseek failed");

		return static_cast<std::uint64_t>(pos);
	}

	bool get_file_size(std::uint64_t& size_out) override
	{
		struct stat st {};

		if (::fstat(this->fd, &st) != 0)
			return false;

		if (st.st_size() < 0)
			return false;

		size_out = static_cast<std::uint64_t>(st.st_size());
		return true;
	}
};


bool durable_rename( // Rename file on POSIX.
	const NativePath& from, // Source path.
	const NativePath& to, // Destination path.
	bool replace_existing // Whether replacement is allowed.
)
{
	if (!replace_existing) // If caller does not allow replacement.
	{
		struct stat st {}; // Buffer for stat result.

		if (::stat(to.c_str(), &st) == 0) // Destination already exists.
		{
			return false; // Refuse to replace.
		}
	}

	return ::rename( // POSIX rename.
		from.c_str(), // Old path.
		to.c_str() // New path.
	) == 0; // Return true on success.
}

static NativePath parent_directory_of(const NativePath& path) // Extract parent directory.
{
	const std::size_t pos = path.find_last_of('/'); // Find final slash.

	if (pos == NativePath::npos) // No slash found.
	{
		return "."; // Parent is current directory.
	}

	if (pos == 0) // File under root directory.
	{
		return "/"; // Parent is root.
	}

	return path.substr(0, pos); // Return path before final slash.
}

bool sync_directory(const NativePath& directory_path) // Flush directory metadata on POSIX.
{
#ifdef O_DIRECTORY // Some POSIX systems provide O_DIRECTORY.
	const int flags = O_RDONLY | O_DIRECTORY; // Open only if path is a directory.
#else
	const int flags = O_RDONLY; // Portable fallback.
#endif

	int fd = ::open( // Open directory.
		directory_path.c_str(), // Directory path.
		flags // Open flags.
	);

	if (fd == -1) // Opening directory failed.
	{
		return false; // Report failure.
	}

	bool ok = false; // Track fsync result.

	while (true) // fsync may be interrupted.
	{
		const int result = ::fsync(fd); // Flush directory metadata.

		if (result == 0) // Success.
		{
			ok = true; // Mark success.
			break; // Exit loop.
		}

		if (errno == EINTR) // Interrupted by signal.
		{
			continue; // Retry.
		}

		ok = false; // Real failure.
		break; // Exit loop.
	}

	const int close_result = ::close(fd); // Close directory descriptor.

	return ok && close_result == 0; // Both fsync and close should succeed.
}

bool sync_parent_directory(const NativePath& file_path) // Sync parent directory of file.
{
	return sync_directory(parent_directory_of(file_path)); // Extract parent and fsync it.
}

#endif


std::unique_ptr<ReadableFile> open_readable_file(const std::filesystem::path& path)
{
#ifdef _WIN32
	return std::make_unique<WindowsReadableFile>(path);
#else 
	return std::make_unique<PosixReadableFile>(path);
#endif
}

std::unique_ptr<WritableFile> open_writable_file(const std::filesystem::path& path)
{
#ifdef _WIN32
	return std::make_unique<WindowsWritableFile>(path);
#else
	return std::make_unique<PosixWritableFile>(path);
#endif
}

//#ifdef _WIN32
//
//bool get_file_size(ReadableFile& file, std::uint64_t& size_out)
//{
//	LARGE_INTEGER li{};
//
//	if (!::GetFileSizeEx(file.handle, &li))
//		return false;
//
//	if (li.QuadPart < 0)
//		return false;
//
//	size_out = static_cast<std::uint64_t>(li.QuadPart);
//	return true;
//}
//
//#else
//
//bool get_file_size(int fd, std::uint64_t& size_out)
//{
//	struct stat st {};
//	
//	if (::fstat(fd, &st) != 0)
//		return false;
//
//	if (st.st_size() < 0)
//		return false;
//
//	size_out = static_cast<std::uint64_t>(st.st_size());
//	return true;
//}
//
//#endif