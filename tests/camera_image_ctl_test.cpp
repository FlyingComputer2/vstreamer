#include "components/camera_image_ctl.hpp"

#include <gtest/gtest.h>

namespace camctl = vstreamer::detail::camctl;

namespace
{
/* The OV5647 1080p mode at 24 MHz xclk. */
constexpr double k_pclk = 81666700.0 * 24.0 / 25.0;
constexpr int k_hts = 2271;
constexpr int k_vts = 1134;
}  // namespace

TEST(CameraImageCtlTest, FindsKeys)
{
    EXPECT_GE(camctl::find_key("exposure_us"), 0);
    EXPECT_GE(camctl::find_key("sharpness"), 0);
    EXPECT_EQ(-1, camctl::find_key("size"));
    EXPECT_EQ(-1, camctl::find_key(""));
}

TEST(CameraImageCtlTest, ValueRanges)
{
    const int gain = camctl::find_key("gain");
    EXPECT_TRUE(camctl::value_ok(gain, 0));
    EXPECT_FALSE(camctl::value_ok(gain, 8));
    EXPECT_TRUE(camctl::value_ok(gain, 16));
    EXPECT_TRUE(camctl::value_ok(gain, 1023));
    EXPECT_FALSE(camctl::value_ok(gain, 1024));

    const int gamma = camctl::find_key("gamma");
    EXPECT_TRUE(camctl::value_ok(gamma, 0));
    EXPECT_FALSE(camctl::value_ok(gamma, 10));
    EXPECT_TRUE(camctl::value_ok(gamma, 72));

    const int br = camctl::find_key("brightness");
    EXPECT_TRUE(camctl::value_ok(br, -128));
    EXPECT_FALSE(camctl::value_ok(br, -129));
    EXPECT_FALSE(camctl::value_ok(-1, 0));
}

TEST(CameraImageCtlTest, ExposureRegisterRoundTrip)
{
    /* One line is ~28.97 us. */
    const uint32_t reg = camctl::ov5647_exposure_reg(10000, k_hts, k_vts, k_pclk);
    EXPECT_EQ(0u, reg & 0x0f);
    EXPECT_EQ(345u, reg >> 4);
    EXPECT_NEAR(10000, camctl::ov5647_exposure_us(reg, k_hts, k_pclk), 15);
}

TEST(CameraImageCtlTest, ExposureClampsToFrame)
{
    EXPECT_EQ(1u << 4, camctl::ov5647_exposure_reg(1, k_hts, k_vts, k_pclk));
    EXPECT_EQ(static_cast<uint32_t>(k_vts - 4) << 4,
              camctl::ov5647_exposure_reg(1000000, k_hts, k_vts, k_pclk));
    /* Unknown timing: one line, never a stretched frame. */
    EXPECT_EQ(16u, camctl::ov5647_exposure_reg(10000, 0, 0, k_pclk));
}

TEST(CameraImageCtlTest, AeTargetMatchesDriverDefault)
{
    /* The driver's default 80 gives the 1080p table's window less a step. */
    const camctl::ae_target_regs t = camctl::ov5647_ae_target(80);
    EXPECT_EQ(86, t.high);
    EXPECT_EQ(73, t.low);
    EXPECT_EQ(172, t.fast_high);
    EXPECT_EQ(36, t.fast_low);
    EXPECT_EQ(255, camctl::ov5647_ae_target(235).fast_high);
}

TEST(CameraImageCtlTest, GammaCurve)
{
    uint8_t x[camctl::k_gamma_points];
    uint8_t y[camctl::k_gamma_points];
    camctl::gamma_curve(100, x, y);
    int prev = 0;
    for (int i = 0; i < camctl::k_gamma_points; ++i)
    {
        const int delta = x[i] - prev + (i == camctl::k_gamma_points - 1 ? 1 : 0);
        EXPECT_EQ(0, delta & (delta - 1)) << "x delta at " << i << " is not a power of two";
        EXPECT_EQ(x[i], y[i]);
        prev = x[i];
    }
    EXPECT_EQ(255, x[camctl::k_gamma_points - 1]);

    camctl::gamma_curve(50, x, y);
    EXPECT_GT(y[0], x[0]);
    EXPECT_EQ(255, y[camctl::k_gamma_points - 1]);
    camctl::gamma_curve(200, x, y);
    EXPECT_LT(y[0], x[0]);
}
