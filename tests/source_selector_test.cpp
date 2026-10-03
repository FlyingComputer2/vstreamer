#include "apps/common/tx/source_selector.hpp"

#include "core/component_source.hpp"
#include "core/data_packet.hpp"
#include "core/packet_types.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cerrno>
#include <thread>
#include <vector>

namespace
{

class fake_source : public vstreamer::component_source
{
public:
    int open_rc = 0;
    int next_output = -ENODEV;
    vstreamer::media_kind_e frame_kind = vstreamer::media_kind_e::MJPEG;

    void set_output_sequence(std::vector<int> seq)
    {
        output_seq = std::move(seq);
        output_idx = 0;
    }

    [[nodiscard]] std::string name() const override
    {
        return "fake";
    }

    [[nodiscard]] vstreamer::media_kind_e output_kind() const override
    {
        return frame_kind;
    }

    int open() override
    {
        return open_rc;
    }

    void close() override {}

    int output(uint8_t /*port*/, vstreamer::data_packet &out, int /*timeout_ms*/) override
    {
        int code = next_output;
        if (!output_seq.empty())
        {
            code = output_seq[output_idx % output_seq.size()];
            output_idx++;
        }
        if (0 == code)
        {
            auto fd = std::make_unique<vstreamer::frame_data>();
            fd->kind = frame_kind;
            out.reset(std::move(fd));
        }
        return code;
    }

    int configure(std::string_view /*key*/, std::string_view /*value*/) override
    {
        return -ENOTSUP;
    }

    int query(std::string_view key, std::string *value) const override
    {
        if (nullptr == value)
        {
            return -EINVAL;
        }
        if (key == "width")
        {
            *value = "640";
            return 0;
        }
        if (key == "height")
        {
            *value = "480";
            return 0;
        }
        if (key == "fps")
        {
            *value = "30";
            return 0;
        }
        return -ENOTSUP;
    }

private:
    std::vector<int> output_seq;
    size_t           output_idx = 0;
};

}  // namespace

TEST(SourceSelectorTest, CameraEnodevFallsBackToNoiseOnce)
{
    fake_source camera;
    fake_source noise;
    noise.frame_kind = vstreamer::media_kind_e::NV12;
    camera.set_output_sequence({-ENODEV});
    noise.next_output = 0;

    int                      switch_count = 0;
    int                      mjpeg_count = 0;
    int                      nv12_count = 0;
    vstreamer::apps::tx::source_selector sel(
        camera, noise, 320, 240, 25,
        [&](vstreamer::apps::tx::source_kind kind, int w, int h, int f) {
            switch_count++;
            EXPECT_EQ(vstreamer::apps::tx::source_kind::noise_fallback, kind);
            EXPECT_EQ(320, w);
            EXPECT_EQ(240, h);
            EXPECT_EQ(25, f);
        },
        [&](vstreamer::data_packet &&) { mjpeg_count++; },
        [&](vstreamer::data_packet &&) { nv12_count++; });

    ASSERT_EQ(0, sel.open());
    EXPECT_EQ(vstreamer::apps::tx::source_kind::camera, sel.active_kind());
    EXPECT_EQ(0, sel.poll_once(0));
    EXPECT_EQ(vstreamer::apps::tx::source_kind::noise_fallback, sel.active_kind());
    EXPECT_EQ(1, switch_count);
    EXPECT_EQ(0, mjpeg_count);
    EXPECT_EQ(1, nv12_count);
}

TEST(SourceSelectorTest, CameraRecoversAfterNoise)
{
    fake_source camera;
    fake_source noise;
    noise.frame_kind = vstreamer::media_kind_e::NV12;
    camera.set_output_sequence({-ENODEV});
    noise.next_output = 0;

    int switch_count = 0;
    int mjpeg_count = 0;
    vstreamer::apps::tx::source_selector sel(
        camera, noise, 640, 480, 30,
        [&](vstreamer::apps::tx::source_kind kind, int w, int h, int f) {
            switch_count++;
            if (vstreamer::apps::tx::source_kind::camera == kind)
            {
                EXPECT_EQ(640, w);
                EXPECT_EQ(480, h);
                EXPECT_EQ(30, f);
            }
        },
        [&](vstreamer::data_packet &&) { mjpeg_count++; },
        [&](vstreamer::data_packet &&) {});

    ASSERT_EQ(0, sel.open());
    ASSERT_EQ(0, sel.poll_once(0));
    EXPECT_EQ(1, switch_count);

    camera.set_output_sequence({0});
    std::this_thread::sleep_for(std::chrono::milliseconds(510));
    ASSERT_EQ(0, sel.poll_once(0));
    EXPECT_EQ(2, switch_count);
    EXPECT_EQ(1, mjpeg_count);
}

TEST(SourceSelectorTest, CameraAbsentAtStartUsesNoise)
{
    fake_source camera;
    fake_source noise;
    noise.frame_kind = vstreamer::media_kind_e::NV12;
    camera.open_rc = -ENODEV;
    noise.next_output = 0;

    int switch_count = 0;
    vstreamer::apps::tx::source_selector sel(
        camera, noise, 640, 480, 30,
        [&](vstreamer::apps::tx::source_kind kind, int, int, int) {
            switch_count++;
            EXPECT_EQ(vstreamer::apps::tx::source_kind::noise_fallback, kind);
        },
        {}, [&](vstreamer::data_packet &&) {});

    ASSERT_EQ(0, sel.open());
    EXPECT_EQ(1, switch_count);
    EXPECT_EQ(vstreamer::apps::tx::source_kind::noise_fallback, sel.active_kind());
    std::string err;
    ASSERT_EQ(0, sel.query("camera_error", &err));
    EXPECT_FALSE(err.empty());
}

TEST(SourceSelectorTest, NonEnodevErrorDoesNotSwitch)
{
    fake_source camera;
    fake_source noise;
    camera.set_output_sequence({-EIO});
    int switch_count = 0;
    vstreamer::apps::tx::source_selector sel(
        camera, noise, 640, 480, 30,
        [&](vstreamer::apps::tx::source_kind, int, int, int) { switch_count++; }, {}, {});
    ASSERT_EQ(0, sel.open());
    EXPECT_EQ(-EIO, sel.poll_once(0));
    EXPECT_EQ(0, switch_count);
}

TEST(SourceSelectorTest, NoFlappingMoreThanOneSwitchPer500ms)
{
    fake_source camera;
    fake_source noise;
    noise.frame_kind = vstreamer::media_kind_e::NV12;
    camera.set_output_sequence({-ENODEV, 0, -ENODEV, 0, -ENODEV, 0});
    noise.next_output = 0;

    int switch_count = 0;
    vstreamer::apps::tx::source_selector sel(
        camera, noise, 640, 480, 30,
        [&](vstreamer::apps::tx::source_kind, int, int, int) { switch_count++; },
        [&](vstreamer::data_packet &&) {}, [&](vstreamer::data_packet &&) {});
    ASSERT_EQ(0, sel.open());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < deadline)
    {
        (void)sel.poll_once(0);
    }
    EXPECT_LE(switch_count, 1);
}
