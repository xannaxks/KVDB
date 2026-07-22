#include <gtest/gtest.h>

#include "compaction_options.h"

TEST(CompactionOptionsTest, DefaultsAreValid)
{
    CompactionOptions options;
    EXPECT_TRUE(options.validate().is_ok());
}

TEST(CompactionOptionsTest, RejectsTooFewLevels)
{
    CompactionOptions options;
    options.max_levels = 1;
    EXPECT_FALSE(options.validate().is_ok());
}

TEST(CompactionOptionsTest, RejectsZeroL0Trigger)
{
    CompactionOptions options;
    options.l0_file_count_trigger = 0;
    EXPECT_FALSE(options.validate().is_ok());
}

TEST(CompactionOptionsTest, RejectsShortVectors)
{
    CompactionOptions options;
    options.max_bytes_per_level.resize(2);
    EXPECT_FALSE(options.validate().is_ok());

    options = CompactionOptions{};
    options.target_file_size_per_level.resize(2);
    EXPECT_FALSE(options.validate().is_ok());
}

TEST(CompactionOptionsTest, RejectsZeroCompactableLevelLimit)
{
    CompactionOptions options;
    options.max_bytes_per_level[2] = 0;
    EXPECT_FALSE(options.validate().is_ok());
}

TEST(CompactionOptionsTest, RejectsZeroOutputSize)
{
    CompactionOptions options;
    options.target_file_size_per_level[3] = 0;
    EXPECT_FALSE(options.validate().is_ok());
}
