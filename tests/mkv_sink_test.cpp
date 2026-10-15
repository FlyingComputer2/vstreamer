#include "components/mkv_sink.hpp"

#include "core/component_pdu.hpp"
#include "core/sdu_caps.hpp"
#include "core/shared_sized_buffer.hpp"

#include <gtest/gtest.h>

#include "test_pdu_helpers.hpp"

#include "core/mkv_writer.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
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

/* Runs a shell command; returns its stdout, or false when it fails. */
bool run_cmd(const std::string &cmd, std::string *out)
{
    FILE *p = popen(cmd.c_str(), "r");
    if (nullptr == p)
    {
        return false;
    }
    std::string text;
    char        buf[4096];
    size_t      n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0)
    {
        text.append(buf, n);
    }
    const int status = pclose(p);
    if (nullptr != out)
    {
        *out = text;
    }
    return 0 == status;
}

bool have_tool(const char *name)
{
    return run_cmd(std::string("command -v ") + name + " >/dev/null 2>&1", nullptr);
}

/* Demuxes with ffprobe (an independent reader): packet count, end of last packet, keyframes. */
bool probe_video_span(const std::string &path, int *frame_count, double *duration_sec,
                      int *key_count = nullptr)
{
    if (nullptr == frame_count || nullptr == duration_sec)
    {
        return false;
    }
    *frame_count = 0;
    *duration_sec = 0.0;
    if (nullptr != key_count)
    {
        *key_count = 0;
    }

    std::string text;
    if (!run_cmd("ffprobe -v error -select_streams v:0 -show_entries "
                 "packet=pts_time,duration_time,flags -of csv=p=0 '" +
                     path + "'",
                 &text))
    {
        return false;
    }
    std::istringstream in(text);
    std::string        line;
    double             last_end = 0.0;
    while (std::getline(in, line))
    {
        if (line.empty())
        {
            continue;
        }
        double pts = 0.0;
        double dur = 0.0;
        char   flags[16] = {};
        if (std::sscanf(line.c_str(), "%lf,%lf,%15s", &pts, &dur, flags) < 2)
        {
            return false;
        }
        ++(*frame_count);
        last_end = std::max(last_end, pts + dur);
        if (nullptr != key_count && 'K' == flags[0])
        {
            ++(*key_count);
        }
    }
    *duration_sec = last_end;
    return true;
}

/* Container Duration from the Segment Info (what players show). */
bool probe_format_duration(const std::string &path, double *duration_sec)
{
    std::string text;
    if (!run_cmd("ffprobe -v error -show_entries format=duration -of csv=p=0 '" + path + "'",
                 &text))
    {
        return false;
    }
    return 1 == std::sscanf(text.c_str(), "%lf", duration_sec);
}

/* Decodes every frame; true when ffmpeg reports no decode errors. */
bool decodes_cleanly(const std::string &path, int *decoded)
{
    std::string text;
    if (!run_cmd("ffprobe -v error -count_frames -select_streams v:0 -show_entries "
                 "stream=nb_read_frames -of csv=p=0 '" +
                     path + "' 2>/dev/null",
                 &text) ||
        1 != std::sscanf(text.c_str(), "%d", decoded))
    {
        return false;
    }
    /* Rockchip ffmpeg builds log an MPP probe line on every run; it is not a decode error. */
    std::string errors;
    (void)run_cmd("ffmpeg -v error -i '" + path + "' -f null - 2>&1 | grep -v mpp_platform",
                  &errors);
    return errors.empty();
}

std::vector<uint8_t> read_file(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

/*
 * Real H.264 from libx264: 320x240, GOP 10, SPS/PPS on every IDR, AUD before each AU so the
 * Annex-B stream splits into access units on AUD start codes.
 */
bool make_h264_aus(const std::filesystem::path &dir, int frames,
                   std::vector<std::vector<uint8_t>> *aus)
{
    const std::string out = (dir / "in.h264").string();
    char              cmd[1024];
    std::snprintf(cmd, sizeof(cmd),
                  "ffmpeg -v error -y -f lavfi -i testsrc=size=320x240:rate=30 -frames:v %d "
                  "-c:v libx264 -preset ultrafast -bf 0 -g 10 -keyint_min 10 -sc_threshold 0 "
                  "-x264-params repeat-headers=1:aud=1 -f h264 '%s' 2>&1",
                  frames, out.c_str());
    if (!run_cmd(cmd, nullptr))
    {
        return false;
    }
    const std::vector<uint8_t> es = read_file(out);
    std::vector<size_t>        starts;
    for (size_t i = 0; i + 4 < es.size(); ++i)
    {
        if (es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 0 && es[i + 3] == 1 &&
            (es[i + 4] & 0x1F) == 9)
        {
            starts.push_back(i);
        }
    }
    starts.push_back(es.size());
    aus->clear();
    for (size_t k = 0; k + 1 < starts.size(); ++k)
    {
        aus->emplace_back(es.begin() + static_cast<std::ptrdiff_t>(starts[k]),
                          es.begin() + static_cast<std::ptrdiff_t>(starts[k + 1]));
    }
    return static_cast<int>(aus->size()) == frames;
}

bool au_has_idr(const std::vector<uint8_t> &au)
{
    for (size_t i = 0; i + 3 < au.size(); ++i)
    {
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 1 && (au[i + 3] & 0x1F) == 5)
        {
            return true;
        }
    }
    return false;
}

std::filesystem::path fresh_dir(const char *name)
{
    const auto      base = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(base, ec);
    return base;
}

#define REQUIRE_FFPROBE()                                                                        \
    do                                                                                           \
    {                                                                                            \
        if (!have_tool("ffprobe"))                                                               \
        {                                                                                        \
            GTEST_SKIP() << "ffprobe not installed";                                             \
        }                                                                                        \
    } while (0)

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
    pdu.ts_us = static_cast<uint64_t>(frame_index * 33333);
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
    ASSERT_EQ(0, sink.query("inport-0.caps-2.sdu_type", &val));
    EXPECT_EQ("H264_AU", val);
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

TEST(MkvSinkTest, MicrosecondTimestampsDuration)
{
    REQUIRE_FFPROBE();
    const auto base = std::filesystem::temp_directory_path() / "mkv_sink_ts_us";
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(base, ec);

    const std::string out_template = (base / "clip.mkv").string();
    const std::string seg_path = (base / "clip-001.mkv").string();
    mkv_sink sink;
    ASSERT_EQ(sink.open(), 0);
    ASSERT_EQ(sink.configure("queue_depth", "128"), 0);
    ASSERT_EQ(sink.configure("output", out_template.c_str()), 0);
    ASSERT_EQ(sink.configure("fps", "30"), 0);
    ASSERT_EQ(sink.input(make_mjpeg_caps(320, 240)), 0);

    for (int i = 0; i < 30; ++i)
    {
        ASSERT_EQ(sink.input(make_mjpeg_pdu(320, 240, i)), 0);
    }
    sink.close();

    int    frames = 0;
    double dur = 0.0;
    ASSERT_TRUE(std::filesystem::exists(seg_path));
    ASSERT_TRUE(probe_video_span(seg_path, &frames, &dur));
    EXPECT_EQ(frames, 30);
    EXPECT_NEAR(dur, 1.0, 0.1);
    double fmt_dur = 0.0;
    ASSERT_TRUE(probe_format_duration(seg_path, &fmt_dur));
    EXPECT_NEAR(fmt_dur, 1.0, 0.1);

    std::filesystem::remove_all(base, ec);
}

TEST(MkvSinkTest, DurationAndResizeSegments)
{
    REQUIRE_FFPROBE();
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

TEST(MkvSinkTest, H264StartsAtKeyframeAndDecodes)
{
    REQUIRE_FFPROBE();
    const auto base = fresh_dir("mkv_sink_h264");
    std::vector<std::vector<uint8_t>> aus;
    if (!make_h264_aus(base, 40, &aus))
    {
        GTEST_SKIP() << "ffmpeg with libx264 not available";
    }

    const std::string out_template = (base / "clip.mkv").string();
    const std::string seg_path = (base / "clip-001.mkv").string();
    mkv_sink          sink;
    ASSERT_EQ(sink.open(), 0);
    ASSERT_EQ(sink.configure("queue_depth", "128"), 0);
    ASSERT_EQ(sink.configure("output", out_template.c_str()), 0);
    ASSERT_EQ(sink.configure("fps", "30"), 0);
    ASSERT_EQ(sink.input(make_mjpeg_caps(320, 240)), 0);

    /* Join mid-GOP: AU 0 (IDR) is never seen, so AUs 1..9 cannot be decoded. */
    for (size_t i = 1; i < aus.size(); ++i)
    {
        component_pdu pdu;
        pdu.ts_us = static_cast<uint64_t>(1000000 + i * 33333);
        pdu.sdu_type = vstreamer::sdu_type_e::H264_AU;
        pdu.flags = au_has_idr(aus[i]) ? static_cast<uint8_t>(vstreamer::pdu_flag_e::KEY) : 0;
        pdu.sdu = shared_sized_buffer::copy_from(aus[i].data(), aus[i].size());
        ASSERT_EQ(sink.input(std::move(pdu)), 0);
    }
    sink.close();

    std::string val;
    ASSERT_EQ(0, sink.query("skipped", &val));
    EXPECT_EQ("9", val);

    int    frames = 0;
    int    keys = 0;
    double dur = 0.0;
    ASSERT_TRUE(probe_video_span(seg_path, &frames, &dur, &keys));
    EXPECT_EQ(frames, 30);
    EXPECT_EQ(keys, 3);
    EXPECT_NEAR(dur, 1.0, 0.1);

    std::string codec;
    ASSERT_TRUE(run_cmd("ffprobe -v error -select_streams v:0 -show_entries "
                        "stream=codec_name,width,height -of csv=p=0 '" +
                            seg_path + "'",
                        &codec));
    EXPECT_EQ("h264,320,240\n", codec);

    int decoded = 0;
    EXPECT_TRUE(decodes_cleanly(seg_path, &decoded));
    EXPECT_EQ(decoded, 30);

    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}

TEST(MkvWriterTest, TruncatedFilePlaysUpToLastCluster)
{
    REQUIRE_FFPROBE();
    const auto        base = fresh_dir("mkv_writer_truncated");
    const std::string path = (base / "live.mkv").string();
    const std::string copy = (base / "cut.mkv").string();

    vstreamer::mkv_writer       w;
    vstreamer::mkv_track_config cfg;
    cfg.codec = vstreamer::mkv_codec_e::MJPEG;
    cfg.width = 320;
    cfg.height = 240;
    cfg.fps = 30;
    ASSERT_EQ(0, w.open(path, cfg));
    /* 2.5 s: two closed (flushed) clusters, the third still open. */
    for (int i = 0; i < 75; ++i)
    {
        ASSERT_EQ(0, w.write_frame(k_minimal_jpeg, sizeof(k_minimal_jpeg), i * 33333, 33333,
                                   true));
    }

    /* Snapshot what reached the file while recording, as after a power cut. */
    std::filesystem::copy_file(path, copy);
    int    frames = 0;
    double dur = 0.0;
    ASSERT_TRUE(probe_video_span(copy, &frames, &dur));
    EXPECT_GE(frames, 60);

    ASSERT_EQ(0, w.close());
    ASSERT_TRUE(probe_video_span(path, &frames, &dur));
    EXPECT_EQ(frames, 75);
    EXPECT_NEAR(dur, 2.5, 0.1);

    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}
