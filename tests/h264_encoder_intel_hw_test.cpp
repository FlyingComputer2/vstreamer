#include "components/h264_encoder_intel.hpp"

#include "core/key_util.hpp"
#include "core/shared_sized_buffer.hpp"

#include <gtest/gtest.h>

#include "test_pdu_helpers.hpp"

#include <cerrno>
#include <cstdint>
#include <vector>

namespace
{

struct nal_mask
{
    bool idr = false;
    bool sps = false;
    bool pps = false;
};

nal_mask scan_nals(const uint8_t *d, size_t n)
{
    nal_mask m;
    for (size_t i = 0; i + 3 < n; ++i)
    {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)
        {
            const int t = d[i + 3] & 0x1f;
            m.idr = m.idr || t == 5;
            m.sps = m.sps || t == 7;
            m.pps = m.pps || t == 8;
            i += 2;
        }
    }
    return m;
}

int cfg(vstreamer::component &c, const char *key, const char *val)
{
    return c.configure(key, val);
}

vstreamer::component_pdu make_nv12_gradient(int w, int h, uint64_t ts_us, int frame_idx)
{
    const size_t y_sz = static_cast<size_t>(w) * static_cast<size_t>(h);
    const size_t uv_sz = y_sz / 2U;
    std::vector<uint8_t> storage(y_sz + uv_sz);
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const int v = (x + y + frame_idx * 3) & 0xff;
            storage[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] =
                static_cast<uint8_t>(v);
        }
    }
    for (size_t i = 0; i < uv_sz; ++i)
    {
        storage[y_sz + i] = static_cast<uint8_t>((i + frame_idx * 7) & 0xff);
    }

    vstreamer::component_pdu pkt = vstreamer::test_pdu::make_nv12(w, h, ts_us);
    pkt.sdu = vstreamer::shared_sized_buffer::copy_from(storage.data(), storage.size());
    return pkt;
}

double encode_window_bps(vstreamer::h264_encoder_intel &enc, int w, int h, int frames, int fps)
{
    int64_t              bits = 0;
    std::vector<uint64_t> out_ts;
    for (int i = 0; i < frames; ++i)
    {
        vstreamer::component_pdu in = make_nv12_gradient(w, h, static_cast<uint64_t>(i), i);
        if (enc.input(std::move(in)) < 0)
        {
            return -1.0;
        }
        for (;;)
        {
            vstreamer::component_pdu out;
            const int              orv = enc.output(out);
            if (orv == -EAGAIN)
            {
                break;
            }
            if (orv < 0)
            {
                return -1.0;
            }
            if (out.sdu_type != vstreamer::sdu_type_e::H264_AU)
            {
                continue;
            }
            bits += static_cast<int64_t>(out.sdu.size()) * 8;
            out_ts.push_back(out.ts_us);
        }
    }
    for (int spin = 0; spin < 50; ++spin)
    {
        vstreamer::component_pdu out;
        const int              orv = enc.output(out);
        if (orv < 0)
        {
            break;
        }
        if (out.sdu_type != vstreamer::sdu_type_e::H264_AU)
        {
            continue;
        }
        bits += static_cast<int64_t>(out.sdu.size()) * 8;
        out_ts.push_back(out.ts_us);
    }
    if (out_ts.size() < 2)
    {
        return -1.0;
    }
    for (size_t i = 1; i < out_ts.size(); ++i)
    {
        EXPECT_GE(out_ts[i], out_ts[i - 1]);
    }
    const double seconds = static_cast<double>(frames) / static_cast<double>(fps);
    return static_cast<double>(bits) / seconds;
}

}  // namespace

TEST(H264EncoderIntelHwTest, CbrGopAndLiveBitrate)
{
    vstreamer::h264_encoder_intel enc;
    if (cfg(enc, "size", "1920x1080") < 0 || cfg(enc, "fps", "30") < 0 ||
        cfg(enc, "rc", "cbr") < 0 || cfg(enc, "cbr", "1000000") < 0 || cfg(enc, "gop", "120") < 0)
    {
        GTEST_SKIP() << "configure failed";
    }
    if (enc.open() < 0)
    {
        GTEST_SKIP() << "VA-API encoder open failed";
    }

    constexpr int k_w = 1920;
    constexpr int k_h = 1080;
    constexpr int k_gop = 120;
    int           key_count = 0;
    int           frames_since_key = 0;

    ASSERT_EQ(0, enc.input(vstreamer::test_pdu::make_nv12_caps(k_w, k_h, 30)));

    for (int i = 0; i < 300; ++i)
    {
        vstreamer::component_pdu in = make_nv12_gradient(k_w, k_h, static_cast<uint64_t>(i), i);
        ASSERT_EQ(enc.input(std::move(in)), 0);
        for (;;)
        {
            vstreamer::component_pdu out;
            const int              orv = enc.output(out);
            if (orv == -EAGAIN)
            {
                break;
            }
            ASSERT_EQ(orv, 0);
            if (out.sdu_type != vstreamer::sdu_type_e::H264_AU)
            {
                continue;
            }
            const bool key = (out.flags & static_cast<uint8_t>(vstreamer::pdu_flag_e::KEY)) != 0;
            const nal_mask m = scan_nals(out.sdu.u8(), out.sdu.size());
            EXPECT_EQ(key, m.idr) << "AU " << i;
            if (m.idr)
            {
                EXPECT_TRUE(m.sps && m.pps) << "IDR AU " << i << " lacks SPS/PPS";
            }
            if (key)
            {
                if (key_count > 0)
                {
                    EXPECT_EQ(frames_since_key, k_gop - 1);
                }
                ++key_count;
                frames_since_key = 0;
            }
            else
            {
                ++frames_since_key;
            }
        }
    }
    EXPECT_GE(key_count, 2);
    EXPECT_TRUE(key_count >= 1);

    const double bps1 = encode_window_bps(enc, k_w, k_h, 300, 30);
    ASSERT_GT(bps1, 0.0);
    EXPECT_NEAR(bps1, 1000000.0, 1000000.0 * 0.25);

    ASSERT_EQ(cfg(enc, "cbr", "15000000"), 0);
    bool saw_key_after = false;
    for (int i = 300; i < 310; ++i)
    {
        vstreamer::component_pdu in = make_nv12_gradient(k_w, k_h, static_cast<uint64_t>(i), i);
        ASSERT_EQ(enc.input(std::move(in)), 0);
        vstreamer::component_pdu out;
        if (enc.output(out) == 0 && out.sdu_type == vstreamer::sdu_type_e::H264_AU)
        {
            if ((out.flags & static_cast<uint8_t>(vstreamer::pdu_flag_e::KEY)) != 0)
            {
                saw_key_after = true;
            }
        }
    }
    EXPECT_TRUE(saw_key_after);

    const double bps2 = encode_window_bps(enc, k_w, k_h, 300, 30);
    ASSERT_GT(bps2, 0.0);
    EXPECT_NEAR(bps2, 15000000.0, 15000000.0 * 0.20);

    enc.close();
}
