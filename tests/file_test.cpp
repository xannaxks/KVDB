#include <gtest/gtest.h>
#include "file.h"
#include "status.h"

#include <filesystem>
#include <cstddef>
#include <string>
#include <vector>
#include <cstring>

namespace fs = std::filesystem;

class FileTest : public ::testing::Test
{
protected:
	fs::path dir;

	void SetUp() override
	{
		dir = fs::temp_directory_path() / "kvdb_file_testing";
		fs::remove_all(dir);
		fs::create_directories(dir);
	}

	void TearDown() override
	{
		fs::remove_all(dir);
	}

	fs::path path(const std::string& name)
	{
		return dir / name;
	}
};

TEST_F(FileTest, OpenReadableNonExistingFileFails)
{
	auto result = open_readable_file(FileTest::path("missing.dat"));

	EXPECT_FALSE(result.is_ok());
}

TEST_F(FileTest, AppendAndReadBack)
{
	auto file_result = open_writable_file(FileTest::path("data.dat"));
	ASSERT_TRUE(file_result.is_ok());

	auto file = std::move(file_result.value);

	std::uint64_t offset = 0;
	std::string data = "hello world";

	Status s = file->append(data.data(), data.size(), offset);
	ASSERT_TRUE(s.is_ok());

	EXPECT_EQ(offset, static_cast<std::uint64_t>(data.size()));

	s = file->sync();
	ASSERT_TRUE(s.is_ok());

	s = file->close();
	ASSERT_TRUE(s.is_ok());

	auto read_result = open_readable_file(FileTest::path("data.dat"));
	ASSERT_TRUE(read_result.is_ok());

	auto reader = std::move(read_result.value);

	std::vector<char> buffer(data.size());
	std::size_t bytes_read = 0;

	s = reader->read_at(0, buffer.data(), data.size(), bytes_read);
	ASSERT_TRUE(s.is_ok());

	EXPECT_EQ(bytes_read, data.size());
	EXPECT_EQ(std::string(buffer.begin(), buffer.end()), data);
}

TEST_F(FileTest, MultipleAppendsTrackOffsetCorrectly) {
	auto file_result = open_writable_file(path("multi.dat"));
	ASSERT_TRUE(file_result.is_ok());

	auto file = std::move(file_result.value);

	std::uint64_t offset = 0;

	std::string a = "abc";
	std::string b = "defgh";

	ASSERT_TRUE(file->append(a.data(), a.size(), offset).is_ok());
	EXPECT_EQ(offset, 3);

	ASSERT_TRUE(file->append(b.data(), b.size(), offset).is_ok());
	EXPECT_EQ(offset, 8);

	ASSERT_TRUE(file->close().is_ok());

	auto reader_result = open_readable_file(path("multi.dat"));
	ASSERT_TRUE(reader_result.is_ok());

	auto reader = std::move(reader_result.value);

	std::vector<char> buffer(8);
	std::size_t bytes_read = 0;

	ASSERT_TRUE(reader->read_at(0, buffer.data(), 8, bytes_read).is_ok());

	EXPECT_EQ(bytes_read, 8);
	EXPECT_EQ(std::string(buffer.begin(), buffer.end()), "abcdefgh");
}

TEST_F(FileTest, ReadAtOffset) {
	auto writer_result = open_writable_file(path("offset.dat"));
	ASSERT_TRUE(writer_result.is_ok());

	auto writer = std::move(writer_result.value);

	std::uint64_t offset = 0;
	std::string data = "0123456789";

	ASSERT_TRUE(writer->append(data.data(), data.size(), offset).is_ok());
	ASSERT_TRUE(writer->close().is_ok());

	auto reader_result = open_readable_file(path("offset.dat"));
	ASSERT_TRUE(reader_result.is_ok());

	auto reader = std::move(reader_result.value);

	std::vector<char>buffer(4);
	std::size_t bytes_read = 0;

	ASSERT_TRUE(reader->read_at(3, buffer.data(), 4, bytes_read).is_ok());

	EXPECT_EQ(bytes_read, 4);
	EXPECT_EQ(std::string(buffer.begin(), buffer.end()), "3456");
}

TEST_F(FileTest, ReadExactAtSuccess) {
	auto writer_result = open_writable_file(path("exact.dat"));
	ASSERT_TRUE(writer_result.is_ok());

	auto writer = std::move(writer_result.value);

	std::uint64_t offset = 0;
	std::string data = "abcdef";

	ASSERT_TRUE(writer->append(data.data(), data.size(), offset).is_ok());
	ASSERT_TRUE(writer->close().is_ok());

	auto reader_result = open_readable_file(path("exact.dat"));
	ASSERT_TRUE(reader_result.is_ok());

	auto reader = std::move(reader_result.value);

	std::vector<char>buffer(3);
	std::uint64_t track_offset = 0;

	Status s = reader->read_exact_at(2, reinterpret_cast<void*>(buffer.data()), 3, track_offset);

	ASSERT_TRUE(s.is_ok());
	EXPECT_EQ(std::string(buffer.begin(), buffer.end()), "cde");
	EXPECT_EQ(track_offset, 5);
}

TEST_F(FileTest, ReadExactAtUnexpectedEOF) {
	auto writer_result = open_writable_file(path("eof.dat"));
	ASSERT_TRUE(writer_result.is_ok());

	auto writer = std::move(writer_result.value);

	std::uint64_t offset = 0;
	std::string data = "abc";

	ASSERT_TRUE(writer->append(data.data(), data.size(), offset).is_ok());
	ASSERT_TRUE(writer->close().is_ok());

	auto reader_result = open_readable_file(path("eof.dat"));
	ASSERT_TRUE(reader_result.is_ok());

	auto reader = std::move(reader_result.value);

	std::vector<char>buffer(10);
	std::uint64_t track_offset = 0;

	Status s = reader->read_exact_at(0, reinterpret_cast<void*>(buffer.data()), 10, track_offset);

	EXPECT_FALSE(s.is_ok());
	EXPECT_EQ(s.code, StatusCode::UnexpectedEOF);
}

TEST_F(FileTest, GetFileSize) {
	auto writer_result = open_writable_file(path("size.dat"));
	ASSERT_TRUE(writer_result.is_ok());

	auto writer = std::move(writer_result.value);

	std::uint64_t offset = 0;
	std::string data = "123456789";

	ASSERT_TRUE(writer->append(data.data(), data.size(), offset).is_ok());

	std::uint64_t size = 0;
	ASSERT_TRUE(writer->get_file_size(size).is_ok());

	EXPECT_EQ(size, data.size());
}

TEST_F(FileTest, ReopenWritableAppendsToEnd) {
	fs::path p = path("append_existing.dat");

	{
		auto result = open_writable_file(p);
		ASSERT_TRUE(result.is_ok());

		auto file = std::move(result.value);

		std::uint64_t offset = 0;
		std::string data = "abc";

		ASSERT_TRUE(file->append(data.data(), data.size(), offset).is_ok());
		ASSERT_TRUE(file->close().is_ok());
	}

	{
		auto result = open_writable_file(p);
		ASSERT_TRUE(result.is_ok());

		auto file = std::move(result.value);

		auto pos = file->current_position();
		ASSERT_TRUE(pos.is_ok());
		EXPECT_EQ(pos.value, 3);

		std::uint64_t offset = pos.value;
		std::string data = "def";

		ASSERT_TRUE(file->append(data.data(), data.size(), offset).is_ok());
		ASSERT_TRUE(file->close().is_ok());
	}

	auto reader_result = open_readable_file(p);
	ASSERT_TRUE(reader_result.is_ok());

	auto reader = std::move(reader_result.value);

	std::vector<char> buffer(6);
	std::size_t bytes_read = 0;

	ASSERT_TRUE(reader->read_at(0, reinterpret_cast<void*>(buffer.data()), 6, bytes_read).is_ok());

	EXPECT_EQ(bytes_read, 6);
	EXPECT_EQ(std::string(buffer.begin(), buffer.end()), "abcdef");
}

TEST_F(FileTest, DurableRenameMovesFile) {
	fs::path from = path("old.dat");
	fs::path to = path("new.dat");

	auto result = open_writable_file(from);
	ASSERT_TRUE(result.is_ok());

	auto file = std::move(result.value);

	std::uint64_t offset = 0;
	std::string data = "rename me";

	ASSERT_TRUE(file->append(data.data(), data.size(), offset).is_ok());
	ASSERT_TRUE(file->sync().is_ok());

	Status s = file->durable_rename(to, true);
	ASSERT_TRUE(s.is_ok());

	EXPECT_FALSE(fs::exists(from));
	EXPECT_TRUE(fs::exists(to));
}

TEST_F(FileTest, DurableRenameFailsIfDestinationExistsAndNoReplace) {
	fs::path from = path("from.dat");
	fs::path to = path("to.dat");

	{
		auto result = open_writable_file(from);
		ASSERT_TRUE(result.is_ok());
	}

	{
		auto result = open_writable_file(to);
		ASSERT_TRUE(result.is_ok());
	}

	auto result = open_writable_file(from);
	ASSERT_TRUE(result.is_ok());

	auto file = std::move(result.value);

	Status s = file->durable_rename(to, false);

	EXPECT_FALSE(s.is_ok());
}

TEST_F(FileTest, CloseTwiceIsOk) {
	auto result = open_writable_file(path("close.dat"));
	ASSERT_TRUE(result.is_ok());

	auto file = std::move(result.value);

	EXPECT_TRUE(file->close().is_ok());
	EXPECT_TRUE(file->close().is_ok());
}