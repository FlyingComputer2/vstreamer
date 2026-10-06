#include "apps/common/stage_latency.hpp"

#include <gtest/gtest.h>

namespace vstreamer::apps
{
namespace
{

TEST(StageLatencyTest, NodeLatencyDelta)
{
    EXPECT_DOUBLE_EQ(stage_node_latency_ms(0.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(stage_node_latency_ms(12.5, 0.0), 12.5);
    EXPECT_DOUBLE_EQ(stage_node_latency_ms(40.0, 12.5), 27.5);
    EXPECT_DOUBLE_EQ(stage_node_latency_ms(10.0, 15.0), 0.0);
}

}  // namespace
}  // namespace vstreamer::apps
