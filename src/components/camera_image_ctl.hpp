#ifndef VSTREAMER_COMPONENTS_CAMERA_IMAGE_CTL_HPP
#define VSTREAMER_COMPONENTS_CAMERA_IMAGE_CTL_HPP

/*
 * Image controls of the P4 CSI camera (esp32p4_csi_device) that apply while
 * streaming: the OV5647's exposure, gain and white balance (sensor registers
 * over SCCB) and the P4 ISP's colour, gamma and sharpen blocks. Only the
 * platform-independent parts live here (key table, register arithmetic, gamma
 * curve) so the host tests cover them.
 *
 *   exposure_us   manual exposure time in us; 0 = sensor AEC
 *   gain          manual analog gain x16 (16 = 1x .. 1023); 0 = sensor AGC
 *   gain_ceiling  AGC upper limit x16 (16..1023)
 *   ae_target     AEC brightness target (2..235); the driver loads 80
 *   awb           1 = sensor AWB, 0 = manual gains wb_r / wb_g / wb_b
 *   wb_r wb_g wb_b  manual sensor WB gains x1024 (1024 = 1x, 1..4095)
 *   brightness    ISP colour brightness (-128..127, 0 = none)
 *   contrast      ISP colour contrast (0..255, 128 = 1x)
 *   saturation    ISP colour saturation (0..255, 128 = 1x)
 *   hue           ISP colour hue rotation in degrees (0..360)
 *   gamma         ISP gamma exponent x100: y = x^(gamma/100); 100 = linear,
 *                 below 100 lifts shadows; 0 = gamma block off
 *   sharpness     ISP sharpen strength in % of the tuning's; 0 = off
 *
 * A key never set keeps whatever the sensor table or the ISP tuning gives.
 */

#include <cmath>
#include <cstdint>
#include <string_view>

namespace vstreamer::detail::camctl
{

enum class target : uint8_t
{
    sensor,
    isp,
};

struct key_desc
{
    std::string_view name;
    int lo;
    int hi;
    target where;
};

inline constexpr key_desc k_keys[] = {
    {"exposure_us", 0, 1000000, target::sensor},
    {"gain", 0, 1023, target::sensor},
    {"gain_ceiling", 16, 1023, target::sensor},
    {"ae_target", 2, 235, target::sensor},
    {"awb", 0, 1, target::sensor},
    {"wb_r", 1, 4095, target::sensor},
    {"wb_g", 1, 4095, target::sensor},
    {"wb_b", 1, 4095, target::sensor},
    {"brightness", -128, 127, target::isp},
    {"contrast", 0, 255, target::isp},
    {"saturation", 0, 255, target::isp},
    {"hue", 0, 360, target::isp},
    {"gamma", 0, 400, target::isp},
    {"sharpness", 0, 400, target::isp},
};
inline constexpr int k_key_count = static_cast<int>(sizeof(k_keys) / sizeof(k_keys[0]));

/* Index into k_keys, or -1. */
inline int find_key(std::string_view name)
{
    for (int i = 0; i < k_key_count; ++i)
    {
        if (k_keys[i].name == name)
        {
            return i;
        }
    }
    return -1;
}

/* Range check plus the gaps inside a range: manual gain starts at 1x, and a
 * gamma below 0.2 overflows nothing but is no longer a curve anyone wants. */
inline bool value_ok(int index, int64_t v)
{
    if (index < 0 || index >= k_key_count || v < k_keys[index].lo || v > k_keys[index].hi)
    {
        return false;
    }
    const std::string_view n = k_keys[index].name;
    if (n == "gain")
    {
        return 0 == v || v >= 16;
    }
    if (n == "gamma")
    {
        return 0 == v || v >= 20;
    }
    return true;
}

/* OV5647 exposure registers 0x3500..0x3502 hold lines x16. One line lasts
 * HTS / pclk; manual exposure stays at least one line and at most VTS - 4 so
 * it never stretches the frame. */
inline uint32_t ov5647_exposure_reg(int us, int hts, int vts, double pclk_hz)
{
    if (hts <= 0 || vts <= 5 || pclk_hz <= 0)
    {
        return 16;
    }
    const double line_us = hts * 1e6 / pclk_hz;
    long lines = std::lround(us / line_us);
    lines = lines < 1 ? 1 : lines;
    lines = lines > vts - 4 ? vts - 4 : lines;
    return static_cast<uint32_t>(lines) << 4;
}

inline int ov5647_exposure_us(uint32_t reg, int hts, double pclk_hz)
{
    if (hts <= 0 || pclk_hz <= 0)
    {
        return 0;
    }
    return static_cast<int>(std::lround((reg >> 4) * (hts * 1e6 / pclk_hz)));
}

/* AEC stable window and fast-step window around a target, as esp_cam_sensor's
 * OV5647 driver loads them: 0x3a0f, 0x3a10, 0x3a1b, 0x3a1e, 0x3a11, 0x3a1f. */
struct ae_target_regs
{
    uint8_t high;
    uint8_t low;
    uint8_t fast_high;
    uint8_t fast_low;
};

inline ae_target_regs ov5647_ae_target(int target)
{
    const int high = target * 27 / 25;
    const int low = target * 23 / 25;
    const int fast_high = high * 2 > 255 ? 255 : high * 2;
    return {static_cast<uint8_t>(high > 255 ? 255 : high), static_cast<uint8_t>(low),
            static_cast<uint8_t>(fast_high), static_cast<uint8_t>(low / 2)};
}

/* The P4 ISP gamma curve: 16 points, x steps of 16 ending at 255 (every x
 * delta must be a power of two). */
inline constexpr int k_gamma_points = 16;

inline void gamma_curve(int gamma_x100, uint8_t x[k_gamma_points], uint8_t y[k_gamma_points])
{
    const double e = gamma_x100 > 0 ? gamma_x100 / 100.0 : 1.0;
    for (int i = 0; i < k_gamma_points; ++i)
    {
        const int xi = i == k_gamma_points - 1 ? 255 : (i + 1) * 16;
        const long yi = std::lround(255.0 * std::pow(xi / 255.0, e));
        x[i] = static_cast<uint8_t>(xi);
        y[i] = static_cast<uint8_t>(yi < 0 ? 0 : yi > 255 ? 255 : yi);
    }
}

}  // namespace vstreamer::detail::camctl
#endif
