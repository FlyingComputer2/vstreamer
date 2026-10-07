#include "core/component_pdu.hpp"
#include "core/sdu_caps.hpp"
#include "core/sdu_type.hpp"

#include <gtest/gtest.h>

#include <cerrno>

namespace
{

void expect_round_trip_sdu_type(vstreamer::sdu_type_e t)
{
    const char *name = vstreamer::sdu_type_name(t);
    ASSERT_NE(nullptr, name);
    vstreamer::sdu_type_e parsed = vstreamer::sdu_type_e::UNKNOWN;
    EXPECT_TRUE(vstreamer::parse_sdu_type(name, &parsed));
    EXPECT_EQ(t, parsed);
}

}  // namespace

TEST(ComponentPduTest, FlagsAndCapsPredicate)
{
    vstreamer::component_pdu pdu;
    pdu.flags = static_cast<uint8_t>(vstreamer::pdu_flag_e::KEY) |
                static_cast<uint8_t>(vstreamer::pdu_flag_e::AU_END);
    EXPECT_TRUE(vstreamer::has_flag(pdu, vstreamer::pdu_flag_e::KEY));
    EXPECT_TRUE(vstreamer::has_flag(pdu, vstreamer::pdu_flag_e::AU_END));
    EXPECT_FALSE(vstreamer::has_flag(pdu, vstreamer::pdu_flag_e::EOS));
    EXPECT_TRUE(vstreamer::is_caps(vstreamer::sdu_type_e::CAPS_VIDEO_RAW));
    EXPECT_FALSE(vstreamer::is_caps(vstreamer::sdu_type_e::H264_AU));
}

TEST(ComponentPduTest, SduTypeNameRoundTrip)
{
    expect_round_trip_sdu_type(vstreamer::sdu_type_e::UNKNOWN);
    expect_round_trip_sdu_type(vstreamer::sdu_type_e::NV12);
    expect_round_trip_sdu_type(vstreamer::sdu_type_e::MJPEG);
    expect_round_trip_sdu_type(vstreamer::sdu_type_e::H264_AU);
    expect_round_trip_sdu_type(vstreamer::sdu_type_e::RTP);
    expect_round_trip_sdu_type(vstreamer::sdu_type_e::STREAM_DGRAM);
    expect_round_trip_sdu_type(vstreamer::sdu_type_e::PCM);
    expect_round_trip_sdu_type(vstreamer::sdu_type_e::CAPS_VIDEO_RAW);
    expect_round_trip_sdu_type(vstreamer::sdu_type_e::CAPS_VIDEO_CODED);
    expect_round_trip_sdu_type(vstreamer::sdu_type_e::CAPS_AUDIO);
}

TEST(ComponentPduTest, CapsRoundTrip)
{
    vstreamer::video_raw_caps raw {1920, 1080, 1920, 1088, 30, 1};
    vstreamer::component_pdu pdu =
        vstreamer::make_caps_pdu(vstreamer::sdu_type_e::CAPS_VIDEO_RAW, raw, 12345, 0);
    vstreamer::video_raw_caps out {};
    EXPECT_EQ(0, vstreamer::read_caps(pdu, &out));
    EXPECT_EQ(raw.width, out.width);
    EXPECT_EQ(raw.hor_stride, out.hor_stride);

    vstreamer::video_coded_caps coded {1280, 720, 60, 1};
    pdu = vstreamer::make_caps_pdu(vstreamer::sdu_type_e::CAPS_VIDEO_CODED, coded, 99, 1);
    vstreamer::video_coded_caps coded_out {};
    EXPECT_EQ(0, vstreamer::read_caps(pdu, &coded_out));
    EXPECT_EQ(coded.height, coded_out.height);

    vstreamer::audio_caps aud {48000, 2};
    pdu = vstreamer::make_caps_pdu(vstreamer::sdu_type_e::CAPS_AUDIO, aud, 1, 0);
    vstreamer::audio_caps aud_out {};
    EXPECT_EQ(0, vstreamer::read_caps(pdu, &aud_out));
    EXPECT_EQ(aud.channels, aud_out.channels);
}

TEST(ComponentPduTest, ReadCapsWrongTypeOrSize)
{
    vstreamer::video_raw_caps raw {64, 64, 64, 64, 1, 1};
    vstreamer::component_pdu pdu =
        vstreamer::make_caps_pdu(vstreamer::sdu_type_e::CAPS_VIDEO_RAW, raw, 0, 0);
    pdu.sdu.resize(sizeof(vstreamer::video_raw_caps) - 1);
    vstreamer::video_raw_caps out {};
    EXPECT_EQ(-EINVAL, vstreamer::read_caps(pdu, &out));

    vstreamer::video_coded_caps coded {640, 480, 30, 1};
    pdu = vstreamer::make_caps_pdu(vstreamer::sdu_type_e::CAPS_VIDEO_CODED, coded, 0, 0);
    EXPECT_EQ(-EINVAL, vstreamer::read_caps(pdu, &out));
}

TEST(ComponentPduTest, CapsTypeForDataTypes)
{
    EXPECT_EQ(vstreamer::sdu_type_e::CAPS_VIDEO_RAW,
              vstreamer::caps_type_for(vstreamer::sdu_type_e::NV12));
    EXPECT_EQ(vstreamer::sdu_type_e::CAPS_VIDEO_CODED,
              vstreamer::caps_type_for(vstreamer::sdu_type_e::H264_AU));
    EXPECT_EQ(vstreamer::sdu_type_e::CAPS_AUDIO,
              vstreamer::caps_type_for(vstreamer::sdu_type_e::PCM));
}
