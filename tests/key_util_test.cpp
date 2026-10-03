#include "core/key_util.hpp"

#include <gtest/gtest.h>

#include <string>

TEST(KeyUtilTest, ParseDecimal)
{
    int64_t v = 0;
    EXPECT_EQ(0, vstreamer::key_parse_i64("12", &v));
    EXPECT_EQ(12, v);
    EXPECT_EQ(0, vstreamer::key_parse_i64("-3", &v));
    EXPECT_EQ(-3, v);
    EXPECT_EQ(0, vstreamer::key_parse_i64("010", &v));
    EXPECT_EQ(10, v);
}

TEST(KeyUtilTest, RejectsInvalid)
{
    int64_t v = 0;
    EXPECT_LT(vstreamer::key_parse_i64("0x10", &v), 0);
    EXPECT_LT(vstreamer::key_parse_i64("", &v), 0);
    EXPECT_LT(vstreamer::key_parse_i64("12a", &v), 0);
    EXPECT_LT(vstreamer::key_parse_i64("+5", &v), 0);

    const std::string long_digits(100, '9');
    EXPECT_LT(vstreamer::key_parse_i64(long_digits, &v), 0);
}
