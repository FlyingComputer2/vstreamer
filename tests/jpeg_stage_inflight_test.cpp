#include "components/jpeg_decoder_multicore.hpp"

#include "apps/common/tx/tx_stages.hpp"

#include "core/component_input.hpp"
#include "core/component_output.hpp"
#include "core/component_pdu.hpp"
#include "core/sdu_caps.hpp"
#include "core/shared_sized_buffer.hpp"

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

vstreamer::component_pdu make_mjpeg_blob(const uint8_t *data, size_t len)
{
    vstreamer::component_pdu pdu;
    pdu.ts_us = 1000;
    pdu.sdu_type = vstreamer::sdu_type_e::MJPEG;
    pdu.sdu = vstreamer::shared_sized_buffer::copy_from(data, len);
    return pdu;
}

int cfg(vstreamer::component &c, const char *key, const char *val)
{
    return c.configure(key, val);
}

}  // namespace

TEST(JpegStageInflightTest, FailedDecodeOutputReleasesInflight)
{
    vstreamer::jpeg_decoder_multicore jdec;
    ASSERT_EQ(0, cfg(jdec, "size", "320x240"));
    ASSERT_EQ(0, cfg(jdec, "workers", "1"));
    if (jdec.open() < 0)
    {
        GTEST_SKIP() << "jpeg decoder open failed";
    }
    auto *jdec_in = dynamic_cast<vstreamer::component_input *>(&jdec);
    auto *jdec_out = dynamic_cast<vstreamer::component_output *>(&jdec);
    ASSERT_NE(nullptr, jdec_in);
    ASSERT_NE(nullptr, jdec_out);

    ASSERT_EQ(0, jdec_in->input(make_coded_caps(320, 240)));
    const uint8_t corrupt[] = {0x00, 0x01, 0x02, 0x03};
    int             inflight = 0;
    const int       max_inflight = 2;
    for (int i = 0; i < max_inflight + 2; ++i)
    {
        while (inflight < max_inflight)
        {
            ASSERT_EQ(0, jdec_in->input(make_mjpeg_blob(corrupt, sizeof(corrupt))));
            inflight++;
        }
        vstreamer::component_pdu out;
        for (;;)
        {
            const int or_out = jdec_out->output(out);
            if (0 == or_out)
            {
                if (out.sdu_type == vstreamer::sdu_type_e::CAPS_VIDEO_RAW)
                {
                    continue;
                }
                inflight--;
                break;
            }
            if (-EAGAIN == or_out)
            {
                break;
            }
            inflight--;
            break;
        }
    }
    EXPECT_LE(inflight, max_inflight);
    jdec.close();
}
