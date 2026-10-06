#include "apps/common/stage_latency.hpp"

#include <gtest/gtest.h>

namespace vstreamer::apps
{
namespace
{

TEST(StageLatencyTest, MonoIntervalMs)
{
    EXPECT_DOUBLE_EQ(mono_interval_ms(0, 100), 0.0);
    EXPECT_DOUBLE_EQ(mono_interval_ms(1'000'000, 500'000), 0.0);
    EXPECT_DOUBLE_EQ(mono_interval_ms(1'000'000, 13'500'000), 12.5);
}

}  // namespace
}  // namespace vstreamer::apps
