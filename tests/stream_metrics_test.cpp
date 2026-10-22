#include "core/stream_metrics.hpp"

#include <gtest/gtest.h>

using namespace vstreamer;

TEST(StreamMetricsTest, RejectsMalformedAndOverflowingTuples)
{
    stream_metric_tuple tuple{1, 2, 3};
    for (const char *text :
         {"", "1,2", "1,2,3,4", "1,2,", "1,,3", "-1,2,3", "+1,2,3", " 1,2,3",
          "1,2,3junk", "1,2,18446744073709551616"})
    {
        EXPECT_FALSE(stream_metric_parse(text, &tuple)) << text;
        EXPECT_EQ(1u, tuple.timestamp_ms);
        EXPECT_EQ(2u, tuple.first);
        EXPECT_EQ(3u, tuple.second);
    }
    EXPECT_TRUE(stream_metric_parse("123,0,18446744073709551615", &tuple));
    EXPECT_EQ(UINT64_MAX, tuple.second);
}

TEST(StreamMetricsTest, RateUsesSampleTimeAndResetsOnSessionChange)
{
    stream_metric_rate rate;
    double first = -1, second = -1;
    EXPECT_TRUE(rate.sample({1000, 100, 200}, 1, &first, &second));
    EXPECT_DOUBLE_EQ(0., first);
    EXPECT_TRUE(rate.sample({1250, 110, 220}, 1, &first, &second));
    EXPECT_DOUBLE_EQ(40., first);
    EXPECT_DOUBLE_EQ(80., second);
    EXPECT_FALSE(rate.sample({1250, 115, 230}, 1, &first, &second));
    EXPECT_TRUE(rate.sample({1500, 120, 240}, 1, &first, &second));
    EXPECT_DOUBLE_EQ(40., first);
    EXPECT_TRUE(rate.sample({1500, 999, 999}, 2, &first, &second));
    EXPECT_DOUBLE_EQ(0., first);
    EXPECT_TRUE(rate.sample({1750, 10, 20}, 2, &first, &second));
    EXPECT_DOUBLE_EQ(0., first);
}
