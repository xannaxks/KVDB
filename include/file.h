#pragma once
#ifdef _WIN32

	#ifndef NOMINMAX
		#define NOMINMAX
	#endif

	#ifndef WIN32_LEAN_AND_MEAN
		#define WIN32_LEAN_AND_MEAN
	#endif

#endif
#include "status.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <filesystem>

class ReadableFile
{
public:
	const std::filesystem::path path;

	virtual ~ReadableFile() = default;

	virtual Status read_at(
		std::uint64_t offset,
		void* buffer,
		std::size_t size,
		std::size_t& bytes_read
	) = 0;

	Status read_exact_at(
		std::uint64_t offset,
		void* buffer,
		std::size_t size,
		std::uint64_t& track_offset
	);

	virtual Status close() = 0;

	virtual Status get_file_size(std::uint64_t& size_out) = 0;
};

class WritableFile
{
public:
	const std::filesystem::path path;

	virtual ~WritableFile() = default;

	virtual Status append(
		const void* data,
		std::size_t size, 
		std::uint64_t& track_offset
	) = 0;

	virtual Status sync() = 0;

	virtual Status close() = 0;

	virtual Result<std::uint64_t> current_position() = 0;

	virtual Status get_file_size(std::uint64_t& size_out) = 0;

	virtual Status durable_rename(const std::filesystem::path& to, bool replace_existing) = 0;

	virtual Status sync_parent_directory() = 0;

	virtual std::filesystem::path parent_directory() = 0;
};

Result<std::unique_ptr<ReadableFile>> open_readable_file(
	const std::filesystem::path& path
);

Result<std::unique_ptr<WritableFile>> open_writable_file(
	const std::filesystem::path& path
);

Result<std::unique_ptr<WritableFile>> open_writable_file(const std::filesystem::path& path);
Result<std::unique_ptr<ReadableFile>> open_readable_file(const std::filesystem::path& path);

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

