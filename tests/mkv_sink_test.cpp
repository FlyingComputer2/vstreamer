#include "components/mkv_sink.hpp"

#include "core/component_pdu.hpp"
#include "core/sdu_caps.hpp"
#include "core/shared_sized_buffer.hpp"

#include <gtest/gtest.h>

#include "test_pdu_helpers.hpp"

#include <cerrno>

extern "C"
{
#include <libavformat/avformat.h>
}

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using vstreamer::component_pdu;
using vstreamer::mkv_sink;
using vstreamer::shared_sized_buffer;

namespace
{

/* Minimal valid 1x1 JPEG (SOI … EOI). */
constexpr uint8_t k_minimal_jpeg[] = {
    0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 0x4a, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x00, 0xff, 0xdb, 0x00, 0x43, 0x00, 0x08, 0x06, 0x06, 0x07, 0x06,
    0x05, 0x08, 0x07, 0x07, 0x07, 0x09, 0x09, 0x08, 0x0a, 0x0c, 0x14, 0x0d, 0x0c, 0x0b, 0x0b,
    0x0c, 0x19, 0x12, 0x13, 0x0f, 0x14, 0x1d, 0x1a, 0x1f, 0x1e, 0x1d, 0x1a, 0x1c, 0x1c, 0x20,
    0x24, 0x2e, 0x27, 0x20, 0x22, 0x2c, 0x23, 0x1c, 0x1c, 0x28, 0x37, 0x29, 0x2c, 0x30, 0x31,
    0x34, 0x34, 0x34, 0x1f, 0x27, 0x39, 0x3d, 0x38, 0x32, 0x3c, 0x2e, 0x33, 0x34, 0x32, 0xff,
    0xc0, 0x00, 0x0b, 0x08, 0x00, 0x01, 0x00, 0x01, 0x01, 0x01, 0x11, 0x00, 0xff, 0xc4, 0x00,
    0x1f, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
    0xff, 0xc4, 0x00, 0xb5, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05,
    0x04, 0x04, 0x00, 0x00, 0x01, 0x7d, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21,
    0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a,
    0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56,
    0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93,
    0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9,
    0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6,
    0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
    0xf8, 0xf9, 0xfa, 0xff, 0xda, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3f, 0x00, 0xfb, 0xd5,
    0xdb, 0x20, 0xa8, 0xa8, 0xa8, 0xff, 0xd9,
};

bool probe_video_span(const std::string &path, int *frame_count, double *duration_sec)
{
    if (nullptr == frame_count || nullptr == duration_sec)
    {
        return false;
    }
    *frame_count = 0;
    *duration_sec = 0.0;

    AVFormatContext *ctx = nullptr;
    if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) < 0 || nullptr == ctx)
    {
        return false;
    }
    if (avformat_find_stream_info(ctx, nullptr) < 0)
    {
        avformat_close_input(&ctx);
        return false;
    }
    int vindex = -1;
    for (unsigned i = 0; i < ctx->nb_streams; ++i)
    {
        if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            vindex = static_cast<int>(i);
            break;
        }
    }
    if (vindex < 0)
    {
        avformat_close_input(&ctx);
        return false;
    }
    const AVRational tb = ctx->streams[vindex]->time_base;

    AVPacket *pkt = av_packet_alloc();
    if (nullptr == pkt)
    {
        avformat_close_input(&ctx);
        return false;
    }
    int64_t last_end = 0;
    while (av_read_frame(ctx, pkt) >= 0)
    {
        if (pkt->stream_index == vindex)
        {
            ++(*frame_count);
            if (pkt->pts >= 0)
            {
                const int64_t dur = pkt->duration > 0 ? pkt->duration : 1;
                last_end = std::max(last_end, pkt->pts + dur);
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&ctx);
    *duration_sec = static_cast<double>(last_end) * av_q2d(tb);
    return true;
}

vstreamer::component_pdu make_mjpeg_caps(int w, int h)
{
    vstreamer::video_coded_caps caps {};
    caps.width = w;
    caps.height = h;
    return vstreamer::make_caps_pdu(vstreamer::sdu_type_e::CAPS_VIDEO_CODED, caps, 0, 0);
}

vstreamer::component_pdu make_mjpeg_pdu(int w, int h, int64_t frame_index)
{
    vstreamer::component_pdu pdu;
    pdu.ts_us = static_cast<uint64_t>(frame_index);
    pdu.sdu_type = vstreamer::sdu_type_e::MJPEG;
    pdu.port = 0;
    pdu.sdu = shared_sized_buffer::copy_from(k_minimal_jpeg, sizeof(k_minimal_jpeg));
    return pdu;
}

}  // namespace

TEST(MkvSinkTest, PortCapsAdvertisesMjpeg)
{
    mkv_sink    sink;
    std::string val;
    ASSERT_EQ(0, sink.query("inport-0.caps-0.sdu_type", &val));
    EXPECT_EQ("CAPS_VIDEO_CODED", val);
    ASSERT_EQ(0, sink.query("inport-0.caps-1.sdu_type", &val));
    EXPECT_EQ("MJPEG", val);
}

TEST(MkvSinkTest, PduInputEagainWhenQueueFull)
{
    mkv_sink sink;
    ASSERT_EQ(0, sink.configure("queue_depth", "1"));
    ASSERT_EQ(sink.open(), 0);
    const auto base = std::filesystem::temp_directory_path() / "mkv_sink_pdu_eagain";
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(base, ec);
    const std::string out_template = (base / "clip.mkv").string();
    ASSERT_EQ(sink.configure("output", out_template.c_str()), 0);

    ASSERT_EQ(0, sink.input(make_mjpeg_caps(320, 240)));
    bool saw_eagain = false;
    for (int i = 0; i < 512; ++i)
    {
        const int rc = sink.input(make_mjpeg_pdu(320, 240, i));
        if (-EAGAIN == rc)
        {
            saw_eagain = true;
            break;
        }
        ASSERT_EQ(0, rc);
    }
    EXPECT_TRUE(saw_eagain);

    sink.close();
    std::filesystem::remove_all(base, ec);
}

TEST(MkvSinkTest, DurationAndResizeSegments)
{
    const auto base = std::filesystem::temp_directory_path() / "mkv_sink_test";
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(base, ec);

    const std::string out_template = (base / "clip.mkv").string();
    const std::string seg1 = (base / "clip-001.mkv").string();
    const std::string seg2 = (base / "clip-002.mkv").string();

    mkv_sink sink;
    ASSERT_EQ(sink.open(), 0);
    ASSERT_EQ(sink.configure("queue_depth", "128"), 0);
    ASSERT_EQ(sink.configure("output", out_template.c_str()), 0);
    ASSERT_EQ(sink.configure("fps", "30"), 0);

    for (int i = 0; i < 60; ++i)
    {
        const int w = (i < 30) ? 320 : 640;
        const int h = (i < 30) ? 240 : 480;
        if (0 == i || i == 30)
        {
            ASSERT_EQ(sink.input(make_mjpeg_caps(w, h)), 0);
        }
        ASSERT_EQ(sink.input(make_mjpeg_pdu(w, h, i)), 0);
    }
    sink.close();

    ASSERT_TRUE(std::filesystem::exists(seg1));
    ASSERT_TRUE(std::filesystem::exists(seg2));
    ASSERT_FALSE(std::filesystem::exists(out_template));

    int         frames1 = 0;
    int         frames2 = 0;
    double      d1 = 0.0;
    double      d2 = 0.0;
    ASSERT_TRUE(probe_video_span(seg1, &frames1, &d1));
    ASSERT_TRUE(probe_video_span(seg2, &frames2, &d2));
    EXPECT_EQ(frames1, 30);
    EXPECT_EQ(frames2, 30);
    EXPECT_NEAR(d1, 1.0, 0.15);
    EXPECT_NEAR(d2, 1.0, 0.15);

    std::filesystem::remove_all(base, ec);
}
