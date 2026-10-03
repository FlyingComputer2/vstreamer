#include "test_app/stream_sdl/cpu_map.hpp"

#include <gtest/gtest.h>

namespace vstreamer::test_app
{
namespace
{

TEST(CpuMapTest, DefaultsWhenEmpty)
{
    const cpu_stage_map m = parse_cpu_map("");
    EXPECT_EQ(m.source, 0);
    EXPECT_EQ(m.jpeg, 1);
    EXPECT_EQ(m.encode, 2);
    EXPECT_EQ(m.rx, 3);
    EXPECT_EQ(m.jpeg_workers.size(), 4U);
    EXPECT_EQ(m.jpeg_workers[0], 4);
    EXPECT_EQ(m.jpeg_workers[3], 7);
}

TEST(CpuMapTest, OverrideOneStageAndRange)
{
    const cpu_stage_map m = parse_cpu_map("encode=5;jpeg_workers=4-5");
    EXPECT_EQ(m.encode, 5);
    EXPECT_EQ(m.jpeg_workers.size(), 2U);
    EXPECT_EQ(m.jpeg_workers[0], 4);
    EXPECT_EQ(m.jpeg_workers[1], 5);
}

TEST(CpuMapTest, UnpinRx)
{
    const cpu_stage_map m = parse_cpu_map("rx=-1");
    EXPECT_EQ(m.rx, -1);
    EXPECT_EQ(m.source, 0);
}

TEST(CpuMapTest, MalformedFallsBackToDefaults)
{
    const cpu_stage_map bad = parse_cpu_map("not_a_stage=1");
    const cpu_stage_map def = parse_cpu_map("");
    EXPECT_EQ(bad.source, def.source);
    EXPECT_EQ(bad.jpeg_workers.size(), def.jpeg_workers.size());
}

}  // namespace
}  // namespace vstreamer::test_app
