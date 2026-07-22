#include <gtest/gtest.h>

#include "compaction_record_policy.h"

TEST(CompactionRecordPolicyTest, KeepsPutAtEveryLevel)
{
    EXPECT_TRUE(compaction_keep_newest_record(::Type::Put, false));
    EXPECT_TRUE(compaction_keep_newest_record(::Type::Put, true));
}

TEST(CompactionRecordPolicyTest, KeepsNonBottommostTombstone)
{
    EXPECT_TRUE(compaction_keep_newest_record(::Type::Tombstone, false));
}

TEST(CompactionRecordPolicyTest, DropsBottommostTombstone)
{
    EXPECT_FALSE(compaction_keep_newest_record(::Type::Tombstone, true));
}
