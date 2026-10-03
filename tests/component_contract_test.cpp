#include "core/component.hpp"
#include "core/component_coder.hpp"
#include "core/component_factory.hpp"
#include "core/component_sink.hpp"
#include "core/component_source.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <memory>
#include <string>

namespace
{

constexpr const char k_unknown_key[] = "no_such_key_xyz";

void expect_unknown_key_enotsup(vstreamer::component &c, const std::string &label)
{
    EXPECT_EQ(-ENOTSUP, c.configure(k_unknown_key, "1")) << label << " configure";
    std::string val;
    EXPECT_EQ(-ENOTSUP, c.query(k_unknown_key, &val)) << label << " query";
}

}  // namespace

/* H9: unknown keys return -ENOTSUP (bad values return -EINVAL) on every component. */
TEST(ComponentContractTest, UnknownKeyIsEnotsup)
{
    int checked = 0;
    for (const char *name : {"noise", "v4l2", "stream_receiver"})
    {
        if (auto c = vstreamer::component_factory::create_source(name))
        {
            expect_unknown_key_enotsup(*c, name);
            ++checked;
        }
    }
    for (const char *name : {"jpeg_decoder_multicore", "h264_decoder_mpp", "h264_encoder_mpp",
                             "h264_encoder_cedar", "h264_encoder_intel", "rtp_h264_pay",
                             "rtp_h264_depay"})
    {
        if (auto c = vstreamer::component_factory::create_coder(name))
        {
            expect_unknown_key_enotsup(*c, name);
            ++checked;
        }
    }
    for (const char *name : {"mkv_sink", "stream_sender", "sdl_sink"})
    {
        if (auto c = vstreamer::component_factory::create_sink(name))
        {
            expect_unknown_key_enotsup(*c, name);
            ++checked;
        }
    }
    EXPECT_GT(checked, 0);
}
