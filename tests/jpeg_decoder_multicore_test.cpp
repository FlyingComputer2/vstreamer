#include "components/jpeg_decoder_multicore.hpp"

#include "core/component_pdu.hpp"
#include "core/sdu_caps.hpp"

#include <gtest/gtest.h>

#include <cerrno>

namespace
{

vstreamer::component_pdu make_coded_caps(int w, int h)
{
    vstreamer::video_coded_caps caps {};
    caps.width = w;
    caps.height = h;
    return vstreamer::make_caps_pdu(vstreamer::sdu_type_e::CAPS_VIDEO_CODED, caps, 1, 0);
}

int cfg(vstreamer::component &c, const char *key, const char *val)
{
    return c.configure(key, val);
}

}  // namespace

TEST(JpegDecoderMulticoreTest, ReEmitsOutputCapsOnEachInputCaps)
{
    vstreamer::jpeg_decoder_multicore dec;
    ASSERT_EQ(0, cfg(dec, "size", "320x240"));
    ASSERT_EQ(0, cfg(dec, "workers", "1"));
    if (dec.open() < 0)
    {
        GTEST_SKIP() << "jpeg decoder open failed";
    }
    ASSERT_EQ(0, dec.input(make_coded_caps(320, 240)));
    ASSERT_EQ(0, dec.input(make_coded_caps(320, 240)));

    int caps_out = 0;
    for (int i = 0; i < 4; ++i)
    {
        vstreamer::component_pdu out;
        const int r = dec.output(out);
        if (0 == r && out.sdu_type == vstreamer::sdu_type_e::CAPS_VIDEO_RAW)
        {
            ++caps_out;
        }
        else if (-EAGAIN == r)
        {
            break;
        }
    }
    EXPECT_EQ(2, caps_out);
    dec.close();
}
