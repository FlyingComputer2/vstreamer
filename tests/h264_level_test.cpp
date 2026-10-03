#include "core/h264_level.hpp"

#include <gtest/gtest.h>

namespace vstreamer
{
namespace
{

TEST(H264LevelTest, AnnexATableCases)
{
    EXPECT_EQ(h264_level_for_size(416, 240, 30, 0), 13);
    EXPECT_EQ(h264_level_for_size(416, 240, 30, 2000), 20);
    EXPECT_EQ(h264_level_for_size(1280, 720, 30, 0), 31);
    EXPECT_EQ(h264_level_for_size(1280, 720, 60, 0), 32);
    EXPECT_EQ(h264_level_for_size(1920, 1080, 30, 0), 40);
    EXPECT_EQ(h264_level_for_size(1920, 1080, 60, 0), 42);
    EXPECT_EQ(h264_level_for_size(3840, 2160, 30, 0), 51);
}

}  // namespace
}  // namespace vstreamer
