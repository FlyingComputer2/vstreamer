#include "components/h264_decoder_mpp.hpp"
#include "components/h264_encoder_mpp.hpp"
#include "components/jpeg_decoder_multicore.hpp"

#include "core/component_pdu.hpp"
#include "core/sdu_caps.hpp"
#include "core/sdu_type.hpp"
#include "core/shared_sized_buffer.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <vector>

namespace
{

vstreamer::component_pdu make_raw_caps(int w, int h)
{
    vstreamer::video_raw_caps caps {};
    caps.width = w;
    caps.height = h;
    caps.hor_stride = w;
    caps.ver_stride = h;
    return vstreamer::make_caps_pdu(vstreamer::sdu_type_e::CAPS_VIDEO_RAW, caps, 1, 0);
}

vstreamer::component_pdu make_coded_caps(int w, int h)
{
    vstreamer::video_coded_caps caps {};
    caps.width = w;
    caps.height = h;
    return vstreamer::make_caps_pdu(vstreamer::sdu_type_e::CAPS_VIDEO_CODED, caps, 1, 0);
}

vstreamer::component_pdu make_nv12_pdu(int w, int h)
{
    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 3U / 2U;
    std::vector<uint8_t> storage(bytes, 0x10);
    vstreamer::component_pdu pdu;
    pdu.ts_us = 42;
    pdu.sdu_type = vstreamer::sdu_type_e::NV12;
    pdu.sdu = vstreamer::shared_sized_buffer::copy_from(storage.data(), storage.size());
    return pdu;
}

vstreamer::component_pdu make_mjpeg_pdu()
{
    const uint8_t byte = 0xff;
    vstreamer::component_pdu pdu;
    pdu.ts_us = 1;
    pdu.sdu_type = vstreamer::sdu_type_e::MJPEG;
    pdu.sdu = vstreamer::shared_sized_buffer::copy_from(&byte, 1);
    return pdu;
}

}  // namespace

TEST(H264EncoderMppTest, CapsRejectionPdu)
{
    vstreamer::h264_encoder_mpp enc;
    ASSERT_EQ(0, enc.configure("size", "320x240"));
    ASSERT_EQ(0, enc.open());

    vstreamer::component_pdu bad_caps = make_raw_caps(640, 480);
    EXPECT_EQ(-ENOTSUP, enc.input(std::move(bad_caps)));
    EXPECT_EQ(-ENOTSUP, enc.input(make_nv12_pdu(320, 240)));

    vstreamer::component_pdu good_caps = make_raw_caps(320, 240);
    EXPECT_EQ(0, enc.input(std::move(good_caps)));
    EXPECT_EQ(0, enc.input(make_nv12_pdu(320, 240)));
    enc.close();
}

TEST(H264DecoderMppTest, CapsRejectionPdu)
{
    vstreamer::h264_decoder_mpp dec;
    ASSERT_EQ(0, dec.configure("size", "320x240"));
    ASSERT_EQ(0, dec.open());

    vstreamer::component_pdu bad_caps = make_coded_caps(640, 480);
    EXPECT_EQ(-ENOTSUP, dec.input(std::move(bad_caps)));

    const uint8_t b = 0;
    vstreamer::component_pdu au;
    au.ts_us = 1;
    au.sdu_type = vstreamer::sdu_type_e::H264_AU;
    au.sdu = vstreamer::shared_sized_buffer::copy_from(&b, 1);
    EXPECT_EQ(-ENOTSUP, dec.input(std::move(au)));

    EXPECT_EQ(0, dec.input(make_coded_caps(320, 240)));
    EXPECT_EQ(0, dec.input(std::move(au)));
    dec.close();
}

TEST(JpegDecoderMulticoreTest, CapsRejectionPdu)
{
    vstreamer::jpeg_decoder_multicore dec;
    ASSERT_EQ(0, dec.configure("size", "640x480"));
    ASSERT_EQ(0, dec.open());

    EXPECT_EQ(-ENOTSUP, dec.input(make_coded_caps(1280, 720)));
    EXPECT_EQ(-ENOTSUP, dec.input(make_mjpeg_pdu()));

    EXPECT_EQ(0, dec.input(make_coded_caps(320, 240)));
    EXPECT_EQ(0, dec.input(make_mjpeg_pdu()));
    dec.close();
}
