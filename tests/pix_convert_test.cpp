#include "core/pix_convert.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

namespace vstreamer
{
namespace
{

TEST(PixConvertTest, Nv21TwoByTwoSwapChromaToNv12)
{
    /* 2x2 NV21: Y plane then VU interleaved (stride 2). */
    const uint8_t src[] = {
        10, 20,
        30, 40,
        100, 200,
    };
    uint8_t dst[12] = {};
    const int r =
        pack_yuv420sp_to_nv12(src, 2, 2, 2, 2, dst, 2, 2, true);
    ASSERT_EQ(r, 0);
    EXPECT_EQ(dst[0], 10);
    EXPECT_EQ(dst[1], 20);
    EXPECT_EQ(dst[2], 30);
    EXPECT_EQ(dst[3], 40);
    /* UV row: swapped to NV12 (U then V). */
    EXPECT_EQ(dst[4], 200);
    EXPECT_EQ(dst[5], 100);
}

TEST(PixConvertTest, PadColumnsNoZeroBytes)
{
    /* 4x2 NV12-ish layout: Y (4*2) + UV (4*1). */
    const uint8_t src[] = {
        1, 2, 3, 4,
        5, 6, 7, 8,
        50, 60, 70, 80,
    };
    std::vector<uint8_t> dst(8 * 4 * 3 / 2, 0);
    const int            r =
        pack_yuv420sp_to_nv12(src, 4, 2, 4, 2, dst.data(), 8, 4, false);
    ASSERT_EQ(r, 0);
    for (uint8_t b : dst)
    {
        EXPECT_NE(b, 0);
    }
    /* Last column replicated from column 3 (index 3). */
    EXPECT_EQ(dst[3], dst[7]);
    EXPECT_EQ(dst[3], dst[7]);
}

}  // namespace
}  // namespace vstreamer
