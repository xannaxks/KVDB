#pragma once
#ifdef _WIN32

	#ifndef NOMINMAX
		#define NOMINMAX
	#endif

	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif

#endif
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <filesystem>

class ReadableFile
{
public:
	virtual ~ReadableFile() = default;

	virtual bool read_at(
		std::uint64_t offset,
		void* buffer,
		std::size_t size,
		std::size_t& bytes_read
	) = 0;

	bool read_exact_at(
		std::uint64_t offset,
		void* buffer,
		std::size_t size,
		std::uint64_t& track_offset
	);

	virtual bool close() = 0;

	virtual bool get_file_size(std::uint64_t& size_out) = 0;
};

class WritableFile
{
public:
	virtual ~WritableFile() = default;

	virtual bool append(
		const void* data,
		std::size_t size, 
		std::uint64_t& track_offset
	) = 0;

	virtual bool sync() = 0;

	virtual bool close() = 0;

	virtual std::uint64_t current_position() = 0;

	virtual bool get_file_size(std::uint64_t& size_out) = 0;
};

std::unique_ptr<ReadableFile> open_readable_file(
	const std::filesystem::path& path
);

std::unique_ptr<WritableFile> open_writable_file(
	const std::filesystem::path& path
);

bool durable_rename(const std::filesystem::path& from, const std::filesystem::path& to, bool replace_existing);

bool sync_parent_directory(const std::filesystem::path& path);

std::unique_ptr<WritableFile> open_writable_file(const std::filesystem::path& path);
std::unique_ptr<ReadableFile> open_readable_file(const std::filesystem::path &path);

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

