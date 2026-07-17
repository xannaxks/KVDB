#include <gtest/gtest.h>

#include "file_helpers.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace
{

#define ASSERT_STATUS_OK(expression)                                            \
    do {                                                                        \
        const Status status__ = (expression);                                   \
        ASSERT_TRUE(status__.is_ok())                                           \
            << "status code=" << static_cast<int>(status__.code)               \
            << ", message=" << status__.message;                               \
    } while (false)

#define EXPECT_STATUS_CODE(expression, expected_code)                           \
    do {                                                                        \
        const Status status__ = (expression);                                   \
        EXPECT_FALSE(status__.is_ok());                                         \
        EXPECT_EQ(status__.code, (expected_code))                               \
            << "message=" << status__.message;                                 \
    } while (false)

    class TemporaryPath
    {
    public:
        explicit TemporaryPath(std::string stem)
        {
            static std::atomic<std::uint64_t> counter{ 0 };

            path_ = std::filesystem::temp_directory_path() /
                (std::move(stem) + "_" +
                    std::to_string(counter.fetch_add(1, std::memory_order_relaxed)) +
                    ".tmp");

            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }

        ~TemporaryPath()
        {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }

        const std::filesystem::path& get() const noexcept
        {
            return path_;
        }

    private:
        std::filesystem::path path_;
    };

    std::unique_ptr<WritableFile> open_test_writer(
        const std::filesystem::path& path
    )
    {
        Result<std::unique_ptr<WritableFile>> result = open_writable_file(path);

        EXPECT_TRUE(result.is_ok())
            << "open_writable_file failed: " << result.status.message;

        if (!result.is_ok()) {
            return nullptr;
        }

        EXPECT_NE(result.value, nullptr);
        return std::move(result.value);
    }

    std::unique_ptr<ReadableFile> open_test_reader(
        const std::filesystem::path& path
    )
    {
        Result<std::unique_ptr<ReadableFile>> result = open_readable_file(path);

        EXPECT_TRUE(result.is_ok())
            << "open_readable_file failed: " << result.status.message;

        if (!result.is_ok()) {
            return nullptr;
        }

        EXPECT_NE(result.value, nullptr);
        return std::move(result.value);
    }

    void create_plain_file(const std::filesystem::path& path, std::size_t size)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(stream.is_open());

        const std::vector<char> bytes(size, static_cast<char>(0x11));
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(stream.good());
    }

    std::vector<unsigned char> read_plain_file(
        const std::filesystem::path& path
    )
    {
        std::ifstream stream(path, std::ios::binary);
        EXPECT_TRUE(stream.is_open());

        if (!stream.is_open()) {
            return {};
        }

        return std::vector<unsigned char>(
            std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>()
        );
    }

    void append_prefix(
        WritableFile& file,
        std::uint64_t& offset,
        std::size_t size,
        char value = static_cast<char>(0x11)
    )
    {
        const std::vector<char> bytes(size, value);
        ASSERT_STATUS_OK(file.append(bytes.data(), bytes.size(), offset));
    }

    TEST(FitsInBlockTest, ZeroLengthAlwaysFits)
    {
        EXPECT_TRUE(fits_in_block(7, 0, 8).is_ok());
    }

    TEST(FitsInBlockTest, FitsAtBeginningOfBlock)
    {
        EXPECT_TRUE(fits_in_block(16, 8, 8).is_ok());
    }

    TEST(FitsInBlockTest, FitsExactlyInRemainingBytes)
    {
        EXPECT_TRUE(fits_in_block(13, 3, 8).is_ok());
    }

    TEST(FitsInBlockTest, RejectsRangeCrossingBoundary)
    {
        EXPECT_STATUS_CODE(
            fits_in_block(13, 4, 8),
            StatusCode::SizeExceedsBlockBoundary
        );
    }

    TEST(FitsInBlockTest, UsesOffsetModuloBlockSize)
    {
        EXPECT_TRUE(fits_in_block(1'000'005, 3, 8).is_ok());
        EXPECT_STATUS_CODE(
            fits_in_block(1'000'005, 4, 8),
            StatusCode::SizeExceedsBlockBoundary
        );
    }

    TEST(CanReadRangeTest, AcceptsExactRemainingRange)
    {
        EXPECT_TRUE(can_read_range(7, 3, 10).is_ok());
    }

    TEST(CanReadRangeTest, AcceptsZeroBytesAtEndOfFile)
    {
        EXPECT_TRUE(can_read_range(10, 0, 10).is_ok());
    }

    TEST(CanReadRangeTest, RejectsOffsetPastEndOfFile)
    {
        EXPECT_STATUS_CODE(
            can_read_range(11, 0, 10),
            StatusCode::InvalidOffset
        );
    }

    TEST(CanReadRangeTest, RejectsReadPastEndWithoutOverflow)
    {
        EXPECT_STATUS_CODE(
            can_read_range(9, std::numeric_limits<std::size_t>::max(), 10),
            StatusCode::InvalidReadSize
        );
    }

    TEST(OffsetBoundaryCheckTest, AcceptsOffsetAtEndOfFile)
    {
        EXPECT_TRUE(offset_boundary_check(10, 10).is_ok());
    }

    TEST(OffsetBoundaryCheckTest, RejectsOffsetPastEndOfFile)
    {
        EXPECT_STATUS_CODE(
            offset_boundary_check(11, 10),
            StatusCode::InvalidOffset
        );
    }

    TEST(WritableAlignmentTest, DoesNothingWhenAlreadyAligned)
    {
        TemporaryPath path("writable_align_already_aligned");
        std::unique_ptr<WritableFile> file = open_test_writer(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t offset = 0;
        append_prefix(*file, offset, 8);

        ASSERT_STATUS_OK(align_to_block_boundary(*file, offset, 8));
        EXPECT_EQ(offset, 8u);

        ASSERT_STATUS_OK(file->close());
        EXPECT_EQ(read_plain_file(path.get()).size(), 8u);
    }

    TEST(WritableAlignmentTest, AppendsSmallPoisonPaddingToBoundary)
    {
        TemporaryPath path("writable_align_padding");
        std::unique_ptr<WritableFile> file = open_test_writer(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t offset = 0;
        append_prefix(*file, offset, 5);

        ASSERT_STATUS_OK(align_to_block_boundary(*file, offset, 8));
        EXPECT_EQ(offset, 8u);

        ASSERT_STATUS_OK(file->close());

        const std::vector<unsigned char> bytes = read_plain_file(path.get());
        ASSERT_EQ(bytes.size(), 8u);

        EXPECT_TRUE(std::all_of(
            bytes.begin(),
            bytes.begin() + 5,
            [](unsigned char byte) { return byte == 0x11u; }
        ));

        EXPECT_TRUE(std::all_of(
            bytes.begin() + 5,
            bytes.end(),
            [](unsigned char byte) { return byte == 0xCDu; }
        ));
    }

    TEST(WritableEnsureFitsTest, DoesNotPadWhenRangeAlreadyFits)
    {
        TemporaryPath path("writable_ensure_fits");
        std::unique_ptr<WritableFile> file = open_test_writer(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t offset = 0;
        append_prefix(*file, offset, 5);

        ASSERT_STATUS_OK(ensure_fits_in_block(*file, 3, offset, 8));
        EXPECT_EQ(offset, 5u);

        ASSERT_STATUS_OK(file->close());
        EXPECT_EQ(read_plain_file(path.get()).size(), 5u);
    }

    TEST(WritableEnsureFitsTest, PadsSmallTrailerWhenRequestedRangeCrossesBoundary)
    {
        TemporaryPath path("writable_ensure_padding");
        std::unique_ptr<WritableFile> file = open_test_writer(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t offset = 0;
        append_prefix(*file, offset, 6);

        ASSERT_STATUS_OK(ensure_fits_in_block(*file, 3, offset, 8));
        EXPECT_EQ(offset, 8u);

        ASSERT_STATUS_OK(file->close());

        const std::vector<unsigned char> bytes = read_plain_file(path.get());
        ASSERT_EQ(bytes.size(), 8u);
        EXPECT_EQ(bytes[6], 0xCDu);
        EXPECT_EQ(bytes[7], 0xCDu);
    }

    TEST(WritableEnsureFitsTest, ZeroLengthDoesNotCausePadding)
    {
        TemporaryPath path("writable_zero_length");
        std::unique_ptr<WritableFile> file = open_test_writer(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t offset = 0;
        append_prefix(*file, offset, 7);

        ASSERT_STATUS_OK(ensure_fits_in_block(*file, 0, offset, 8));
        EXPECT_EQ(offset, 7u);

        ASSERT_STATUS_OK(file->close());
        EXPECT_EQ(read_plain_file(path.get()).size(), 7u);
    }

    TEST(WritableEnsureFitsTest, RejectsObjectLargerThanWholeBlock)
    {
        TemporaryPath path("writable_too_large");
        std::unique_ptr<WritableFile> file = open_test_writer(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t offset = 0;

        EXPECT_STATUS_CODE(
            ensure_fits_in_block(*file, 9, offset, 8),
            StatusCode::SizeExceedsBlockSize
        );

        EXPECT_EQ(offset, 0u);
        ASSERT_STATUS_OK(file->close());
    }

    TEST(ReadableAlignmentTest, DoesNothingWhenAlreadyAligned)
    {
        TemporaryPath path("readable_align_already_aligned");
        create_plain_file(path.get(), 16);

        std::unique_ptr<ReadableFile> file = open_test_reader(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t offset = 8;
        ASSERT_STATUS_OK(align_to_block_boundary(*file, offset, 8));
        EXPECT_EQ(offset, 8u);

        ASSERT_STATUS_OK(file->close());
    }

    TEST(ReadableAlignmentTest, AdvancesOffsetToNextBoundary)
    {
        TemporaryPath path("readable_align_next");
        create_plain_file(path.get(), 16);

        std::unique_ptr<ReadableFile> file = open_test_reader(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t offset = 5;
        ASSERT_STATUS_OK(align_to_block_boundary(*file, offset, 8));
        EXPECT_EQ(offset, 8u);

        ASSERT_STATUS_OK(file->close());
    }

    TEST(ReadableEnsureFitsTest, LeavesOffsetWhenRangeFits)
    {
        TemporaryPath path("readable_ensure_fits");
        create_plain_file(path.get(), 16);

        std::unique_ptr<ReadableFile> file = open_test_reader(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t offset = 5;
        ASSERT_STATUS_OK(ensure_fits_in_block(*file, 3, offset, 8));
        EXPECT_EQ(offset, 5u);

        ASSERT_STATUS_OK(file->close());
    }

    TEST(ReadableEnsureFitsTest, AlignsOffsetWhenRangeWouldCrossBoundary)
    {
        TemporaryPath path("readable_ensure_align");
        create_plain_file(path.get(), 16);

        std::unique_ptr<ReadableFile> file = open_test_reader(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t offset = 6;
        ASSERT_STATUS_OK(ensure_fits_in_block(*file, 3, offset, 8));
        EXPECT_EQ(offset, 8u);

        ASSERT_STATUS_OK(file->close());
    }

    TEST(ReadableEnsureFitsTest, RejectsObjectLargerThanWholeBlock)
    {
        TemporaryPath path("readable_too_large");
        create_plain_file(path.get(), 16);

        std::unique_ptr<ReadableFile> file = open_test_reader(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t offset = 6;

        EXPECT_STATUS_CODE(
            ensure_fits_in_block(*file, 9, offset, 8),
            StatusCode::SizeExceedsBlockSize
        );

        EXPECT_EQ(offset, 6u);
        ASSERT_STATUS_OK(file->close());
    }

    TEST(MoveToNextBlockTest, MovesToBoundaryFromMiddleOfBlock)
    {
        TemporaryPath path("move_next_from_middle");
        create_plain_file(path.get(), 24);

        std::unique_ptr<ReadableFile> file = open_test_reader(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t offset = 5;
        ASSERT_STATUS_OK(move_g_to_next_block(*file, offset, 8));
        EXPECT_EQ(offset, 8u);

        ASSERT_STATUS_OK(file->close());
    }

    TEST(MoveToNextBlockTest, AdvancesWholeBlockWhenAlreadyOnBoundary)
    {
        TemporaryPath path("move_next_from_boundary");
        create_plain_file(path.get(), 24);

        std::unique_ptr<ReadableFile> file = open_test_reader(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t offset = 8;
        ASSERT_STATUS_OK(move_g_to_next_block(*file, offset, 8));
        EXPECT_EQ(offset, 16u);

        ASSERT_STATUS_OK(file->close());
    }

    TEST(FileHelpersHardeningTest, FitsInBlockRejectsZeroBlockSize)
    {
        EXPECT_STATUS_CODE(
            fits_in_block(0, 1, 0),
            StatusCode::InvalidArgument
        );
    }

    TEST(FileHelpersHardeningTest, ReadableAlignmentRejectsOffsetOverflow)
    {
        TemporaryPath path("readable_alignment_overflow");
        create_plain_file(path.get(), 1);

        std::unique_ptr<ReadableFile> file = open_test_reader(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t offset = std::numeric_limits<std::uint64_t>::max();

        EXPECT_STATUS_CODE(
            align_to_block_boundary(*file, offset, 4096),
            StatusCode::DataTypeOverflow
        );

        EXPECT_EQ(offset, std::numeric_limits<std::uint64_t>::max());
        ASSERT_STATUS_OK(file->close());
    }

    TEST(FileHelpersHardeningTest, MoveToNextBlockRejectsOffsetOverflow)
    {
        TemporaryPath path("move_next_overflow");
        create_plain_file(path.get(), 1);

        std::unique_ptr<ReadableFile> file = open_test_reader(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t offset = std::numeric_limits<std::uint64_t>::max();

        EXPECT_STATUS_CODE(
            move_g_to_next_block(*file, offset, 4096),
            StatusCode::DataTypeOverflow
        );

        EXPECT_EQ(offset, std::numeric_limits<std::uint64_t>::max());
        ASSERT_STATUS_OK(file->close());
    }

    TEST(FileHelpersHardeningTest, WritableAlignmentRejectsTrackedOffsetMismatch)
    {
        TemporaryPath path("writable_offset_mismatch");
        std::unique_ptr<WritableFile> file = open_test_writer(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t incorrect_offset = 1;

        EXPECT_STATUS_CODE(
            align_to_block_boundary(*file, incorrect_offset, 8),
            StatusCode::InvalidOffset
        );

        EXPECT_EQ(incorrect_offset, 1u);
        ASSERT_STATUS_OK(file->close());
    }




    TEST(FitsInBlockTest, RejectsZeroBlockSizeEvenForZeroLength)
    {
        EXPECT_STATUS_CODE(
            fits_in_block(0, 0, 0),
            StatusCode::InvalidArgument
        );
    }

    TEST(WritableAlignmentTest, RejectsZeroBlockSizeWithoutWriting)
    {
        TemporaryPath path("writable_align_zero_block");
        std::unique_ptr<WritableFile> file = open_test_writer(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t offset = 0;
        EXPECT_STATUS_CODE(
            align_to_block_boundary(*file, offset, 0),
            StatusCode::InvalidArgument
        );
        EXPECT_EQ(offset, 0u);

        ASSERT_STATUS_OK(file->close());
        EXPECT_TRUE(read_plain_file(path.get()).empty());
    }

    TEST(WritableAlignmentTest, AppendsExactlyOneFullChunkOfPadding)
    {
        TemporaryPath path("writable_align_full_chunk");
        std::unique_ptr<WritableFile> file = open_test_writer(path.get());
        ASSERT_NE(file, nullptr);

        constexpr std::uint32_t block_size = 8192;
        std::uint64_t offset = 0;
        append_prefix(*file, offset, 4096);

        ASSERT_STATUS_OK(align_to_block_boundary(*file, offset, block_size));
        EXPECT_EQ(offset, 8192u);

        ASSERT_STATUS_OK(file->close());
        const std::vector<unsigned char> bytes = read_plain_file(path.get());
        ASSERT_EQ(bytes.size(), 8192u);
        EXPECT_TRUE(std::all_of(
            bytes.begin() + 4096,
            bytes.end(),
            [](unsigned char byte) { return byte == 0xCDu; }
        ));
    }

    TEST(WritableAlignmentTest, AppendsPaddingLargerThanOneChunk)
    {
        TemporaryPath path("writable_align_multiple_chunks");
        std::unique_ptr<WritableFile> file = open_test_writer(path.get());
        ASSERT_NE(file, nullptr);

        constexpr std::uint32_t block_size = 8192;
        std::uint64_t offset = 0;
        append_prefix(*file, offset, 1000);

        ASSERT_STATUS_OK(align_to_block_boundary(*file, offset, block_size));
        EXPECT_EQ(offset, 8192u);

        ASSERT_STATUS_OK(file->close());
        const std::vector<unsigned char> bytes = read_plain_file(path.get());
        ASSERT_EQ(bytes.size(), 8192u);
        EXPECT_TRUE(std::all_of(
            bytes.begin() + 1000,
            bytes.end(),
            [](unsigned char byte) { return byte == 0xCDu; }
        ));
    }

    TEST(WritableEnsureFitsTest, RejectsZeroBlockSize)
    {
        TemporaryPath path("writable_ensure_zero_block");
        std::unique_ptr<WritableFile> file = open_test_writer(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t offset = 0;
        EXPECT_STATUS_CODE(
            ensure_fits_in_block(*file, 1, offset, 0),
            StatusCode::InvalidArgument
        );
        EXPECT_EQ(offset, 0u);

        ASSERT_STATUS_OK(file->close());
    }

    TEST(WritableEnsureFitsTest, PadsExactlyOneFullChunkWhenRangeCrossesBoundary)
    {
        TemporaryPath path("writable_ensure_full_chunk");
        std::unique_ptr<WritableFile> file = open_test_writer(path.get());
        ASSERT_NE(file, nullptr);

        constexpr std::uint32_t block_size = 8192;
        std::uint64_t offset = 0;
        append_prefix(*file, offset, 4096);

        ASSERT_STATUS_OK(
            ensure_fits_in_block(*file, 4097, offset, block_size)
        );
        EXPECT_EQ(offset, 8192u);

        ASSERT_STATUS_OK(file->close());
    }

    TEST(ReadableAlignmentTest, RejectsZeroBlockSize)
    {
        TemporaryPath path("readable_align_zero_block");
        create_plain_file(path.get(), 1);
        std::unique_ptr<ReadableFile> file = open_test_reader(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t offset = 0;
        EXPECT_STATUS_CODE(
            align_to_block_boundary(*file, offset, 0),
            StatusCode::InvalidArgument
        );
        EXPECT_EQ(offset, 0u);

        ASSERT_STATUS_OK(file->close());
    }

    TEST(ReadableAlignmentTest, AllowsAlignmentResultEqualToUint64Max)
    {
        TemporaryPath path("readable_align_exact_uint64_max");
        create_plain_file(path.get(), 1);
        std::unique_ptr<ReadableFile> file = open_test_reader(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t offset = std::numeric_limits<std::uint64_t>::max() - 1;
        ASSERT_STATUS_OK(align_to_block_boundary(*file, offset, 3));
        EXPECT_EQ(offset, std::numeric_limits<std::uint64_t>::max());

        ASSERT_STATUS_OK(file->close());
    }

    TEST(ReadableEnsureFitsTest, RejectsZeroBlockSize)
    {
        TemporaryPath path("readable_ensure_zero_block");
        create_plain_file(path.get(), 1);
        std::unique_ptr<ReadableFile> file = open_test_reader(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t offset = 0;
        EXPECT_STATUS_CODE(
            ensure_fits_in_block(*file, 1, offset, 0),
            StatusCode::InvalidArgument
        );
        EXPECT_EQ(offset, 0u);

        ASSERT_STATUS_OK(file->close());
    }

    TEST(MoveToNextBlockTest, RejectsZeroBlockSize)
    {
        TemporaryPath path("move_next_zero_block");
        create_plain_file(path.get(), 1);
        std::unique_ptr<ReadableFile> file = open_test_reader(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t offset = 0;
        EXPECT_STATUS_CODE(
            move_g_to_next_block(*file, offset, 0),
            StatusCode::InvalidArgument
        );
        EXPECT_EQ(offset, 0u);

        ASSERT_STATUS_OK(file->close());
    }

    TEST(MoveToNextBlockTest, AllowsResultEqualToUint64Max)
    {
        TemporaryPath path("move_next_exact_uint64_max");
        create_plain_file(path.get(), 1);
        std::unique_ptr<ReadableFile> file = open_test_reader(path.get());
        ASSERT_NE(file, nullptr);

        std::uint64_t offset = std::numeric_limits<std::uint64_t>::max() - 3;
        ASSERT_STATUS_OK(move_g_to_next_block(*file, offset, 3));
        EXPECT_EQ(offset, std::numeric_limits<std::uint64_t>::max());

        ASSERT_STATUS_OK(file->close());
    }

} // namespaceч