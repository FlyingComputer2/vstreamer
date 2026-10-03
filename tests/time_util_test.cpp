#include "core/time_util.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

namespace
{

constexpr int64_t k_one_ms_ns = 1'000'000LL;

}  // namespace

TEST(TimeUtilTest, RoundTripMonoNearNow)
{
    const int64_t t = vstreamer::steady_mono_ns() - 5 * k_one_ms_ns;
    const int64_t rt = vstreamer::mono_to_realtime_ns(t);
    const int64_t back = vstreamer::realtime_to_mono_ns(rt);
    EXPECT_GE(back, t - k_one_ms_ns);
    EXPECT_LE(back, t + k_one_ms_ns);
}

TEST(TimeUtilTest, MonoToRealtimeNearRealtimeNow)
{
    const int64_t mono_now = vstreamer::steady_mono_ns();
    const int64_t rt_mapped = vstreamer::mono_to_realtime_ns(mono_now);
    const int64_t rt_now = vstreamer::realtime_ns();
    EXPECT_GE(rt_mapped, rt_now - k_one_ms_ns);
    EXPECT_LE(rt_mapped, rt_now + k_one_ms_ns);
}

TEST(TimeUtilTest, NonPositiveInputsMapToZero)
{
    EXPECT_EQ(0, vstreamer::mono_to_realtime_ns(0));
    EXPECT_EQ(0, vstreamer::mono_to_realtime_ns(-1));
    EXPECT_EQ(0, vstreamer::realtime_to_mono_ns(0));
    EXPECT_EQ(0, vstreamer::realtime_to_mono_ns(-1));
}
