#include "test_app/camera_noise_mux.hpp"

#include "test_app/camera_noise_mux_logic.hpp"

#if defined(ENABLE_V4L2_SOURCE) && defined(ENABLE_NOISE_SOURCE)

#include <cerrno>
#include <cstdio>

namespace vstreamer::test_app
{

namespace
{

bool key_is_noise_config(std::string_view key)
{
    return key == "noise-bandwidth" || key == "noise-randomness" || key == "noise-block-size" ||
           key == "pregenerate-frames" || key == "pregenerate-frame" ||
           key == "pregenerate_frames" || key == "pregenerate_frame";
}

bool key_is_noise_query(std::string_view key)
{
    return key_is_noise_config(key) || key == "noise-luma-block-size" || key == "noise-fft-simd" ||
           key == "noise-fft-grid";
}

}  // namespace

camera_noise_mux_source::camera_noise_mux_source(v4l2_source &camera, noise_source &noise)
    : camera_(camera), noise_(noise)
{
}

std::string camera_noise_mux_source::name() const
{
    return "camera_noise_mux";
}

media_kind_e camera_noise_mux_source::output_kind() const
{
    if (noise_active_)
    {
        return noise_.output_kind();
    }
    return camera_.output_kind();
}

int camera_noise_mux_source::open()
{
    noise_active_ = false;
    logged_noise_ = false;
    logged_camera_ = false;
    const int cr = camera_.open();
    if (cr < 0)
    {
        return cr;
    }
    return noise_.open();
}

void camera_noise_mux_source::close()
{
    camera_.close();
    noise_.close();
    noise_active_ = false;
    logged_noise_ = false;
    logged_camera_ = false;
}

int camera_noise_mux_source::output(uint8_t port, data_packet &out, int timeout_ms)
{
    const bool was_noise = noise_active_;
    const int  r =
        camera_noise_mux_output(&camera_, &noise_, &noise_active_, port, out, timeout_ms);
    if (0 == r && was_noise && !noise_active_ && !logged_camera_)
    {
        std::fprintf(stderr, "stream_sdl: camera capture restored\n");
        logged_camera_ = true;
        logged_noise_ = false;
    }
    if (0 == r && noise_active_ && !was_noise && !logged_noise_)
    {
        std::fprintf(stderr, "stream_sdl: camera unavailable; NV12 noise fallback\n");
        logged_noise_ = true;
        logged_camera_ = false;
    }
    return r;
}

int camera_noise_mux_source::configure(std::string_view key, std::string_view value)
{
    if (key_is_noise_config(key))
    {
        return noise_.configure(key, value);
    }
    return camera_.configure(key, value);
}

int camera_noise_mux_source::query(std::string_view key, std::string *value) const
{
    if (nullptr == value)
    {
        return -EINVAL;
    }
    if (key == "state")
    {
        *value = noise_active_ ? "noise_fallback" : "camera";
        return 0;
    }
    if (key_is_noise_query(key))
    {
        return noise_.query(key, value);
    }
    if (noise_active_ && (key == "media_type" || key == "pixel_type"))
    {
        return noise_.query(key, value);
    }
    return camera_.query(key, value);
}

}  // namespace vstreamer::test_app

#endif  // ENABLE_V4L2_SOURCE && ENABLE_NOISE_SOURCE
