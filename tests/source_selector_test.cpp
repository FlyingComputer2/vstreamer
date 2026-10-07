#include "apps/common/tx/source_selector.hpp"

#include "core/component_pdu.hpp"
#include "core/component_source.hpp"
#include "core/pdu_wakeup.hpp"
#include "core/sdu_type.hpp"

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
    vstreamer::sdu_type_e pdu_kind = vstreamer::sdu_type_e::MJPEG;

    void set_output_sequence(std::vector<int> seq)
    {
        output_seq = std::move(seq);
        output_idx = 0;
    }

    [[nodiscard]] std::string name() const override
    {
        return "fake";
    }

    int open() override
    {
        return open_rc;
    }

    void close() override {}

    int output(vstreamer::component_pdu &out) override
    {
        int code = next_output;
        if (!output_seq.empty())
        {
            code = output_seq[output_idx % output_seq.size()];
            output_idx++;
        }
        if (0 != code)
        {
            return code;
        }
        out.ts_us = 1;
        out.seq = 0;
        out.port = 0;
        out.flags = 0;
        out.sdu_type = pdu_kind;
        return 0;
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
    noise.pdu_kind = vstreamer::sdu_type_e::NV12;
    camera.set_output_sequence({-ENODEV});
    noise.next_output = 0;

    int switch_count = 0;
    int pdu_count = 0;
    vstreamer::apps::tx::source_selector sel(camera, noise, 320, 240, 25,
                                            [&](vstreamer::apps::tx::source_kind kind, int w, int h, int f) {
                                                switch_count++;
                                                EXPECT_EQ(vstreamer::apps::tx::source_kind::noise_fallback, kind);
                                                EXPECT_EQ(320, w);
                                                EXPECT_EQ(240, h);
                                                EXPECT_EQ(25, f);
                                            });
    sel.set_push_pdu_handler([&](vstreamer::component_pdu &&) { pdu_count++; });
    sel.bind_source_wakeups(std::make_shared<vstreamer::pdu_wakeup>());

    ASSERT_EQ(0, sel.open());
    EXPECT_EQ(vstreamer::apps::tx::source_kind::camera, sel.active_kind());
    EXPECT_EQ(0, sel.poll_once(-1));
    EXPECT_EQ(vstreamer::apps::tx::source_kind::noise_fallback, sel.active_kind());
    EXPECT_EQ(1, switch_count);
    EXPECT_GE(pdu_count, 1);
}

TEST(SourceSelectorTest, CameraRecoversAfterNoise)
{
    fake_source camera;
    fake_source noise;
    noise.pdu_kind = vstreamer::sdu_type_e::NV12;
    camera.set_output_sequence({-ENODEV});
    noise.next_output = 0;

    int switch_count = 0;
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
        });
    sel.set_push_pdu_handler([&](vstreamer::component_pdu &&) {});
    sel.bind_source_wakeups(std::make_shared<vstreamer::pdu_wakeup>());

    ASSERT_EQ(0, sel.open());
    EXPECT_EQ(0, sel.poll_once(-1));
    EXPECT_EQ(vstreamer::apps::tx::source_kind::noise_fallback, sel.active_kind());

    camera.set_output_sequence({0});
    camera.pdu_kind = vstreamer::sdu_type_e::MJPEG;
    std::this_thread::sleep_for(std::chrono::milliseconds(550));
    EXPECT_EQ(0, sel.poll_once(-1));
    EXPECT_EQ(vstreamer::apps::tx::source_kind::camera, sel.active_kind());
    EXPECT_GE(switch_count, 2);
}

TEST(SourceSelectorTest, CameraAbsentAtStartUsesNoise)
{
    fake_source camera;
    fake_source noise;
    camera.open_rc = -ENODEV;
    noise.pdu_kind = vstreamer::sdu_type_e::NV12;
    noise.next_output = 0;

    int pdu_count = 0;
    vstreamer::apps::tx::source_selector sel(camera, noise, 320, 240, 25, {});
    sel.set_push_pdu_handler([&](vstreamer::component_pdu &&) { pdu_count++; });
    sel.bind_source_wakeups(std::make_shared<vstreamer::pdu_wakeup>());

    ASSERT_EQ(0, sel.open());
    EXPECT_EQ(vstreamer::apps::tx::source_kind::noise_fallback, sel.active_kind());
    EXPECT_EQ(0, sel.poll_once(-1));
    EXPECT_GE(pdu_count, 1);
}
