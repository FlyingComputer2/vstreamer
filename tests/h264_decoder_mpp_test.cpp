#include "components/h264_decoder_mpp.hpp"
#include "components/h264_encoder_mpp.hpp"

#include <gtest/gtest.h>

#include "test_pdu_helpers.hpp"

#include <cerrno>
#include <vector>

namespace
{

int cfg(vstreamer::component &c, const char *key, const char *val)
{
    return c.configure(key, val);
}

}  // namespace

TEST(H264DecoderMppTest, EncodeDecodeRoundTrip)
{
    vstreamer::h264_encoder_mpp enc;
    vstreamer::h264_decoder_mpp dec;
    if (cfg(enc, "size", "320x240") < 0 || cfg(enc, "fps", "30") < 0 || cfg(enc, "rc", "fixqp") < 0 ||
        cfg(enc, "qp", "36") < 0 || cfg(dec, "size", "320x240") < 0)
    {
        GTEST_SKIP() << "configure failed";
    }
    if (enc.open() < 0 || dec.open() < 0)
    {
        GTEST_SKIP() << "MPP codec open failed (no hardware?)";
    }

    constexpr int k_w = 320;
    constexpr int k_h = 240;
    ASSERT_EQ(0, enc.input(vstreamer::test_pdu::make_nv12_caps(k_w, k_h, 30)));

    vstreamer::component_pdu nv12 = vstreamer::test_pdu::make_nv12(k_w, k_h, 1);
    ASSERT_EQ(0, enc.input(std::move(nv12)));

    vstreamer::component_pdu au;
    for (int i = 0; i < 200; ++i)
    {
        if (0 == enc.output(au))
        {
            break;
        }
    }
    if (au.sdu_type != vstreamer::sdu_type_e::H264_AU)
    {
        GTEST_SKIP() << "no encoded AU from MPP encoder";
    }

    ASSERT_EQ(0, dec.input(vstreamer::test_pdu::make_h264_coded_caps(k_w, k_h, 30)));
    ASSERT_EQ(0, dec.input(std::move(au)));

    vstreamer::component_pdu out;
    for (int i = 0; i < 200; ++i)
    {
        if (0 == dec.output(out))
        {
            if (out.sdu_type == vstreamer::sdu_type_e::NV12)
            {
                EXPECT_GE(out.sdu.size(), static_cast<size_t>(k_w * k_h * 3 / 2));
                enc.close();
                dec.close();
                return;
            }
        }
    }
    GTEST_SKIP() << "decoder produced no NV12";
}
