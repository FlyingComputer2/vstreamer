#include "test_app/stream_sdl/camera_noise_mux_logic.hpp"

#include "core/component_source.hpp"
#include "core/data_packet.hpp"
#include "core/packet_types.hpp"

#include <gtest/gtest.h>

#include <cerrno>

namespace
{

class fake_source : public vstreamer::component_source
{
public:
    void set_next_output(int code) { next_ = code; }

    [[nodiscard]] std::string name() const override { return "fake"; }
    [[nodiscard]] vstreamer::media_kind_e output_kind() const override
    {
        return vstreamer::media_kind_e::NV12;
    }
    int open() override { return 0; }
    void close() override {}
    int output(uint8_t /*port*/, vstreamer::data_packet &out, int /*timeout_ms*/) override
    {
        if (0 == next_)
        {
            auto fd = std::make_unique<vstreamer::frame_data>();
            fd->kind = vstreamer::media_kind_e::NV12;
            out.reset(std::move(fd));
        }
        return next_;
    }
    int configure(std::string_view /*key*/, std::string_view /*value*/) override { return -ENOTSUP; }
    int query(std::string_view /*key*/, std::string * /*value*/) const override { return -ENOTSUP; }

private:
    int next_ = -ENODEV;
};

}  // namespace

TEST(CameraNoiseMuxTest, CameraSuccessClearsNoiseFlag)
{
    fake_source camera;
    fake_source noise;
    camera.set_next_output(0);
    noise.set_next_output(0);
    bool noise_active = true;
    vstreamer::data_packet out;
    EXPECT_EQ(0, vstreamer::test_app::camera_noise_mux_output(&camera, &noise, &noise_active, 0,
                                                              out, 0));
    EXPECT_FALSE(noise_active);
}

TEST(CameraNoiseMuxTest, EnodevFallsBackToNoise)
{
    fake_source camera;
    fake_source noise;
    camera.set_next_output(-ENODEV);
    noise.set_next_output(0);
    bool noise_active = false;
    vstreamer::data_packet out;
    EXPECT_EQ(0, vstreamer::test_app::camera_noise_mux_output(&camera, &noise, &noise_active, 0,
                                                              out, 0));
    EXPECT_TRUE(noise_active);
}

TEST(CameraNoiseMuxTest, CameraErrorDoesNotInvokeNoise)
{
    fake_source camera;
    fake_source noise;
    camera.set_next_output(-EIO);
    noise.set_next_output(0);
    bool noise_active = false;
    vstreamer::data_packet out;
    EXPECT_EQ(-EIO, vstreamer::test_app::camera_noise_mux_output(&camera, &noise, &noise_active, 0,
                                                                out, 0));
    EXPECT_FALSE(noise_active);
}
