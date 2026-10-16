#include "components/mkv_sink.hpp"

#include "core/component_pdu.hpp"
#include "core/sdu_caps.hpp"
#include <bfc/shared_sized_buffer.hpp>

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
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

using vstreamer::component_pdu;
using vstreamer::mkv_sink;
using bfc::shared_sized_buffer;

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
    pdu.sdu = bfc::shared_sized_buffer::copy_from(k_minimal_jpeg, sizeof(k_minimal_jpeg));
    return pdu;
}

/* --- Minimal Matroska reader: checks the writer's structure without ffprobe. --- */

struct mkv_block
{
    int64_t  pts_ms = 0;
    bool     key = false;
    size_t   offset = 0; /* payload in the file */
    size_t   size = 0;
};

struct mkv_summary
{
    std::string                doc_type;
    bool                       segment_unknown_size = false;
    bool                       segment_size_ok = false;
    bool                       truncated = false;
    uint64_t                   timestamp_scale = 0;
    double                     duration = -1.0; /* -1: no Duration element */
    std::string                codec_id;
    std::vector<uint8_t>       codec_private;
    uint64_t                   width = 0;
    uint64_t                   height = 0;
    std::vector<mkv_block>     blocks;
    int                        clusters = 0;
    int                        cue_points = 0;
    bool                       cues_point_at_clusters = true;
    std::vector<std::pair<uint32_t, uint64_t>> seeks; /* id, position in segment */
    size_t                     segment_data = 0;
};

constexpr uint32_t k_id_cluster = 0x1F43B675;
constexpr uint32_t k_id_cues = 0x1C53BB6B;

bool read_id(const std::vector<uint8_t> &b, size_t *pos, uint32_t *id)
{
    if (*pos >= b.size() || 0 == b[*pos])
    {
        return false;
    }
    int len = 1;
    while (len <= 4 && !(b[*pos] & (0x80 >> (len - 1))))
    {
        ++len;
    }
    if (len > 4 || *pos + len > b.size())
    {
        return false;
    }
    uint32_t v = 0;
    for (int i = 0; i < len; ++i)
    {
        v = (v << 8) | b[*pos + i];
    }
    *pos += len;
    *id = v;
    return true;
}

bool read_size(const std::vector<uint8_t> &b, size_t *pos, uint64_t *size, bool *unknown)
{
    if (*pos >= b.size() || 0 == b[*pos])
    {
        return false;
    }
    int len = 1;
    while (!(b[*pos] & (0x80 >> (len - 1))))
    {
        ++len;
    }
    if (*pos + len > b.size())
    {
        return false;
    }
    uint64_t v = b[*pos] & (0xFF >> len);
    bool     all_ones = v == (0xFFu >> len);
    for (int i = 1; i < len; ++i)
    {
        v = (v << 8) | b[*pos + i];
        all_ones = all_ones && b[*pos + i] == 0xFF;
    }
    *pos += len;
    *size = v;
    *unknown = all_ones;
    return true;
}

uint64_t read_uint(const std::vector<uint8_t> &b, size_t pos, uint64_t n)
{
    uint64_t v = 0;
    for (uint64_t i = 0; i < n; ++i)
    {
        v = (v << 8) | b[pos + i];
    }
    return v;
}

/* Calls fn(id, data_pos, size) for each child in [pos, end); stops at a truncated element or,
 * when stop_ids is given, at one of those ids (the end of an unknown-size parent). Returns the
 * position where it stopped. */
template <typename Fn>
size_t walk(const std::vector<uint8_t> &b, size_t pos, size_t end, Fn fn, bool *truncated,
            std::initializer_list<uint32_t> stop_ids = {})
{
    while (pos < end)
    {
        size_t   p = pos;
        uint32_t id = 0;
        uint64_t size = 0;
        bool     unknown = false;
        if (!read_id(b, &p, &id))
        {
            *truncated = true;
            return pos;
        }
        for (uint32_t s : stop_ids)
        {
            if (s == id)
            {
                return pos;
            }
        }
        if (!read_size(b, &p, &size, &unknown))
        {
            *truncated = true;
            return pos;
        }
        if (unknown || p + size > b.size())
        {
            const size_t next = fn(id, p, unknown ? SIZE_MAX : size);
            if (!unknown)
            {
                *truncated = true;
                return b.size();
            }
            pos = next;
            continue;
        }
        (void)fn(id, p, size);
        pos = p + size;
    }
    return pos;
}

mkv_summary parse_mkv(const std::vector<uint8_t> &b)
{
    mkv_summary m;
    bool        trunc = false;
    walk(b, 0, b.size(), [&](uint32_t id, size_t data, uint64_t size) -> size_t {
        if (0x1A45DFA3 == id)
        {
            walk(b, data, data + size, [&](uint32_t cid, size_t cd, uint64_t cs) -> size_t {
                if (0x4282 == cid)
                {
                    m.doc_type.assign(b.begin() + cd, b.begin() + cd + cs);
                }
                return cd + cs;
            }, &trunc);
            return data + size;
        }
        if (0x18538067 != id)
        {
            return data + size;
        }
        m.segment_data = data;
        m.segment_unknown_size = SIZE_MAX == size;
        m.segment_size_ok = !m.segment_unknown_size && data + size == b.size();
        const size_t seg_end = m.segment_unknown_size ? b.size() : data + size;
        walk(b, data, seg_end, [&](uint32_t sid, size_t sd, uint64_t ss) -> size_t {
            const size_t s_end = SIZE_MAX == ss ? b.size() : sd + ss;
            switch (sid)
            {
            case 0x114D9B74: /* SeekHead */
                walk(b, sd, s_end, [&](uint32_t, size_t ed, uint64_t es) -> size_t {
                    uint32_t seek_id = 0;
                    uint64_t seek_pos = 0;
                    walk(b, ed, ed + es, [&](uint32_t fid, size_t fd, uint64_t fs) -> size_t {
                        if (0x53AB == fid)
                        {
                            seek_id = static_cast<uint32_t>(read_uint(b, fd, fs));
                        }
                        else if (0x53AC == fid)
                        {
                            seek_pos = read_uint(b, fd, fs);
                        }
                        return fd + fs;
                    }, &trunc);
                    m.seeks.emplace_back(seek_id, seek_pos);
                    return ed + es;
                }, &trunc);
                break;
            case 0x1549A966: /* Info */
                walk(b, sd, s_end, [&](uint32_t fid, size_t fd, uint64_t fs) -> size_t {
                    if (0x2AD7B1 == fid)
                    {
                        m.timestamp_scale = read_uint(b, fd, fs);
                    }
                    else if (0x4489 == fid && 8 == fs)
                    {
                        const uint64_t u = read_uint(b, fd, 8);
                        std::memcpy(&m.duration, &u, sizeof(u));
                    }
                    return fd + fs;
                }, &trunc);
                break;
            case 0x1654AE6B: /* Tracks */
                walk(b, sd, s_end, [&](uint32_t, size_t td, uint64_t ts) -> size_t {
                    walk(b, td, td + ts, [&](uint32_t fid, size_t fd, uint64_t fs) -> size_t {
                        if (0x86 == fid)
                        {
                            m.codec_id.assign(b.begin() + fd, b.begin() + fd + fs);
                        }
                        else if (0x63A2 == fid)
                        {
                            m.codec_private.assign(b.begin() + fd, b.begin() + fd + fs);
                        }
                        else if (0xE0 == fid)
                        {
                            walk(b, fd, fd + fs, [&](uint32_t vid, size_t vd, uint64_t vs) -> size_t {
                                if (0xB0 == vid)
                                {
                                    m.width = read_uint(b, vd, vs);
                                }
                                else if (0xBA == vid)
                                {
                                    m.height = read_uint(b, vd, vs);
                                }
                                return vd + vs;
                            }, &trunc);
                        }
                        return fd + fs;
                    }, &trunc);
                    return td + ts;
                }, &trunc);
                break;
            case k_id_cluster:
            {
                ++m.clusters;
                int64_t cluster_ts = 0;
                return walk(b, sd, s_end, [&](uint32_t fid, size_t fd, uint64_t fs) -> size_t {
                    if (0xE7 == fid)
                    {
                        cluster_ts = static_cast<int64_t>(read_uint(b, fd, fs));
                    }
                    else if (0xA3 == fid && fs >= 4)
                    {
                        mkv_block blk;
                        const auto rel = static_cast<int16_t>((b[fd + 1] << 8) | b[fd + 2]);
                        blk.pts_ms = cluster_ts + rel;
                        blk.key = (b[fd + 3] & 0x80) != 0;
                        blk.offset = fd + 4;
                        blk.size = fs - 4;
                        m.blocks.push_back(blk);
                    }
                    return fd + fs;
                }, &trunc, {k_id_cluster, k_id_cues});
            }
            case k_id_cues:
                walk(b, sd, s_end, [&](uint32_t, size_t pd, uint64_t ps) -> size_t {
                    ++m.cue_points;
                    walk(b, pd, pd + ps, [&](uint32_t fid, size_t fd, uint64_t fs) -> size_t {
                        if (0xB7 == fid)
                        {
                            walk(b, fd, fd + fs, [&](uint32_t qid, size_t qd, uint64_t qs) -> size_t {
                                if (0xF1 == qid)
                                {
                                    size_t   at = m.segment_data + read_uint(b, qd, qs);
                                    uint32_t cid = 0;
                                    m.cues_point_at_clusters = m.cues_point_at_clusters &&
                                                               read_id(b, &at, &cid) &&
                                                               k_id_cluster == cid;
                                }
                                return qd + qs;
                            }, &trunc);
                        }
                        return fd + fs;
                    }, &trunc);
                    return pd + ps;
                }, &trunc);
                break;
            default:
                break;
            }
            return s_end;
        }, &trunc);
        return seg_end;
    }, &trunc);
    m.truncated = trunc;
    return m;
}

/* The SeekHead entry for id points at an element with that id. */
bool seek_ok(const mkv_summary &m, const std::vector<uint8_t> &b, uint32_t id)
{
    for (const auto &[sid, pos] : m.seeks)
    {
        if (sid == id)
        {
            size_t   at = m.segment_data + pos;
            uint32_t got = 0;
            return read_id(b, &at, &got) && got == id;
        }
    }
    return false;
}

std::vector<uint8_t> annexb(std::initializer_list<std::vector<uint8_t>> nals)
{
    std::vector<uint8_t> out;
    for (const auto &n : nals)
    {
        out.insert(out.end(), {0, 0, 0, 1});
        out.insert(out.end(), n.begin(), n.end());
    }
    return out;
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

TEST(MkvWriterTest, MjpegStructure)
{
    const auto        base = fresh_dir("mkv_writer_mjpeg");
    const std::string path = (base / "a.mkv").string();

    vstreamer::mkv_writer       w;
    vstreamer::mkv_track_config cfg;
    cfg.width = 320;
    cfg.height = 240;
    ASSERT_EQ(0, w.open(path, cfg));
    for (int i = 0; i < 75; ++i)
    {
        ASSERT_EQ(0, w.write_frame(k_minimal_jpeg, sizeof(k_minimal_jpeg), i * 33333, 33333,
                                   false));
    }
    ASSERT_EQ(0, w.close());

    const std::vector<uint8_t> b = read_file(path);
    const mkv_summary          m = parse_mkv(b);
    EXPECT_FALSE(m.truncated);
    EXPECT_EQ("matroska", m.doc_type);
    EXPECT_TRUE(m.segment_size_ok);
    EXPECT_EQ(1000000u, m.timestamp_scale);
    EXPECT_NEAR(m.duration, 2500.0, 1.0);
    EXPECT_EQ("V_MJPEG", m.codec_id);
    EXPECT_EQ(320u, m.width);
    EXPECT_EQ(240u, m.height);
    ASSERT_EQ(75u, m.blocks.size());
    for (size_t i = 0; i < m.blocks.size(); ++i)
    {
        EXPECT_TRUE(m.blocks[i].key);
        EXPECT_EQ(static_cast<int64_t>((i * 33333 + 500) / 1000), m.blocks[i].pts_ms);
        ASSERT_EQ(sizeof(k_minimal_jpeg), m.blocks[i].size);
        EXPECT_EQ(0, std::memcmp(b.data() + m.blocks[i].offset, k_minimal_jpeg,
                                 sizeof(k_minimal_jpeg)));
    }
    /* ~1 s clusters, each indexed. */
    EXPECT_EQ(3, m.clusters);
    EXPECT_EQ(3, m.cue_points);
    EXPECT_TRUE(m.cues_point_at_clusters);
    EXPECT_TRUE(seek_ok(m, b, 0x1549A966));
    EXPECT_TRUE(seek_ok(m, b, 0x1654AE6B));
    EXPECT_TRUE(seek_ok(m, b, k_id_cues));

    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}

TEST(MkvWriterTest, H264StructureAvccAndKeyframeStart)
{
    const auto        base = fresh_dir("mkv_writer_h264");
    const std::string path = (base / "a.mkv").string();
    const std::vector<uint8_t> sps = {0x67, 0x42, 0xC0, 0x1E, 0xDA, 0x02};
    const std::vector<uint8_t> pps = {0x68, 0xCE, 0x3C, 0x80};
    const std::vector<uint8_t> aud = {0x09, 0xF0};
    const std::vector<uint8_t> idr = {0x65, 0x88, 0x84, 0x21};
    const std::vector<uint8_t> p_slice = {0x41, 0x9A, 0x02};

    vstreamer::mkv_writer       w;
    vstreamer::mkv_track_config cfg;
    cfg.codec = vstreamer::mkv_codec_e::H264;
    cfg.width = 320;
    cfg.height = 240;
    ASSERT_EQ(0, w.open(path, cfg));
    const auto p0 = annexb({aud, p_slice});
    const auto k1 = annexb({aud, sps, pps, idr});
    const auto p2 = annexb({aud, p_slice});
    /* No keyframe with SPS/PPS yet: skipped, and no file. */
    EXPECT_EQ(1, w.write_frame(p0.data(), p0.size(), 0, 33333, false));
    EXPECT_FALSE(std::filesystem::exists(path));
    /* The IDR NAL makes it a keyframe even without the KEY flag. */
    EXPECT_EQ(0, w.write_frame(k1.data(), k1.size(), 33333, 33333, false));
    EXPECT_EQ(0, w.write_frame(p2.data(), p2.size(), 66667, 33333, false));
    ASSERT_EQ(0, w.close());

    const std::vector<uint8_t> b = read_file(path);
    const mkv_summary          m = parse_mkv(b);
    EXPECT_FALSE(m.truncated);
    EXPECT_EQ("V_MPEG4/ISO/AVC", m.codec_id);

    std::vector<uint8_t> avcc = {1, sps[1], sps[2], sps[3], 0xFF, 0xE1, 0,
                                 static_cast<uint8_t>(sps.size())};
    avcc.insert(avcc.end(), sps.begin(), sps.end());
    avcc.insert(avcc.end(), {1, 0, static_cast<uint8_t>(pps.size())});
    avcc.insert(avcc.end(), pps.begin(), pps.end());
    EXPECT_EQ(avcc, m.codec_private);

    ASSERT_EQ(2u, m.blocks.size());
    EXPECT_TRUE(m.blocks[0].key);
    EXPECT_FALSE(m.blocks[1].key);
    /* Length-prefixed NALs, AUD dropped. */
    std::vector<uint8_t> want;
    for (const auto *n : {&sps, &pps, &idr})
    {
        want.insert(want.end(), {0, 0, 0, static_cast<uint8_t>(n->size())});
        want.insert(want.end(), n->begin(), n->end());
    }
    EXPECT_EQ(want, std::vector<uint8_t>(b.begin() + m.blocks[0].offset,
                                         b.begin() + m.blocks[0].offset + m.blocks[0].size));
    EXPECT_EQ(1, m.cue_points); /* only the keyframe cluster */

    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}

TEST(MkvWriterTest, TruncatedStructureReadable)
{
    const auto        base = fresh_dir("mkv_writer_cut");
    const std::string path = (base / "live.mkv").string();

    vstreamer::mkv_writer       w;
    vstreamer::mkv_track_config cfg;
    cfg.width = 320;
    cfg.height = 240;
    ASSERT_EQ(0, w.open(path, cfg));
    for (int i = 0; i < 75; ++i)
    {
        ASSERT_EQ(0, w.write_frame(k_minimal_jpeg, sizeof(k_minimal_jpeg), i * 33333, 33333,
                                   true));
    }
    /* What is on disk mid-recording: closed clusters are complete, sizes are unknown. */
    const mkv_summary m = parse_mkv(read_file(path));
    EXPECT_TRUE(m.segment_unknown_size);
    EXPECT_LT(m.duration, 0.0);
    EXPECT_GE(m.blocks.size(), 60u);
    EXPECT_EQ("V_MJPEG", m.codec_id);
    ASSERT_EQ(0, w.close());

    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}

TEST(MkvSinkTest, UnchangedFpsKeepsSegment)
{
    const auto        base = fresh_dir("mkv_sink_same_fps");
    const std::string out_template = (base / "clip.mkv").string();

    mkv_sink sink;
    ASSERT_EQ(sink.open(), 0);
    ASSERT_EQ(sink.configure("queue_depth", "128"), 0);
    ASSERT_EQ(sink.configure("output", out_template.c_str()), 0);
    ASSERT_EQ(sink.configure("fps", "30"), 0);
    ASSERT_EQ(sink.input(make_mjpeg_caps(320, 240)), 0);
    for (int i = 0; i < 10; ++i)
    {
        ASSERT_EQ(sink.input(make_mjpeg_pdu(320, 240, i)), 0);
    }
    /* Let the mux thread write them before reconfiguring. */
    for (int t = 0; t < 200; ++t)
    {
        std::string seg;
        if (sink.query("segment", &seg) == 0 && std::filesystem::exists(seg) &&
            std::filesystem::file_size(seg) > 0)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_EQ(sink.configure("fps", "30"), 0);
    for (int i = 10; i < 20; ++i)
    {
        ASSERT_EQ(sink.input(make_mjpeg_pdu(320, 240, i)), 0);
    }
    sink.close();

    EXPECT_FALSE(std::filesystem::exists(base / "clip-002.mkv"));
    const mkv_summary m = parse_mkv(read_file((base / "clip-001.mkv").string()));
    EXPECT_EQ(20u, m.blocks.size());

    std::error_code ec;
    std::filesystem::remove_all(base, ec);
}
