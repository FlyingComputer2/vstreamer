#include "components/h264_decoder_mpp.hpp"
#include "components/h264_encoder_mpp.hpp"

#include "core/data_packet.hpp"
#include "core/packet_types.hpp"
#include "core/shared_sized_buffer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <future>
#include <thread>
#include <utility>
#include <vector>

namespace
{

int cfg(vstreamer::component &c, const char *key, const char *val)
{
    std::string_view k = key;
    std::string_view v = val;
    return c.configure(k, v);
}

vstreamer::data_packet make_nv12(int w, int h, int64_t pts)
{
    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 3U / 2U;
    std::vector<uint8_t> storage(bytes, 0x10);
    auto                 fd = std::make_unique<vstreamer::frame_data>();
    fd->kind = vstreamer::media_kind_e::NV12;
    fd->width = w;
    fd->height = h;
    fd->pts = pts;
    fd->buf = vstreamer::shared_sized_buffer::copy_from(storage.data(), storage.size());
    vstreamer::data_packet pkt;
    pkt.reset(std::move(fd));
    return pkt;
}

}  // namespace

TEST(H264DecoderMppTest, EncodeDecodeRoundTrip)
{
    vstreamer::h264_encoder_mpp enc;
    vstreamer::h264_decoder_mpp dec;
    constexpr int               k_w = 416;
    constexpr int               k_h = 240;
    if (cfg(enc, "size", "416x240") < 0 || cfg(enc, "fps", "30") < 0 || cfg(enc, "rc", "fixqp") < 0 ||
        cfg(enc, "qp", "36") < 0 || cfg(enc, "gop", "30") < 0)
    {
        GTEST_SKIP() << "encoder configure failed";
    }
    if (cfg(dec, "size", "416x240") < 0 || cfg(dec, "fps", "30") < 0)
    {
        GTEST_SKIP() << "decoder configure failed";
    }
    if (enc.open() < 0)
    {
        GTEST_SKIP() << "MPP encoder open failed (no hardware?)";
    }
    if (dec.open() < 0)
    {
        enc.close();
        GTEST_SKIP() << "MPP decoder open failed (no hardware?)";
    }

    auto run_body = [&]() {
        std::vector<vstreamer::data_packet> encoded_aus;
        for (int frame = 0; frame < 30; ++frame)
        {
            const vstreamer::data_packet in = make_nv12(k_w, k_h, frame);
            int                          ir = enc.input(0, in);
            while (ir == -EAGAIN)
            {
                vstreamer::data_packet drain;
                (void)enc.output(0, drain, 1);
                ir = enc.input(0, in);
            }
            if (ir < 0)
            {
                return;
            }
            for (int attempt = 0; attempt < 50; ++attempt)
            {
                vstreamer::data_packet au;
                const int              orv = enc.output(0, au, 20);
                if (orv == 0)
                {
                    encoded_aus.push_back(std::move(au));
                    break;
                }
                if (orv != -EAGAIN)
                {
                    break;
                }
            }
        }

        int decoded = 0;
        for (const auto &au_pkt : encoded_aus)
        {
            int ir = dec.input(0, au_pkt);
            for (int spin = 0; spin < 200 && ir == -EAGAIN; ++spin)
            {
                vstreamer::data_packet nv12;
                const int              orv = dec.output(0, nv12, 5);
                if (orv == 0 && nv12.get_type() == vstreamer::packet_kind_e::FRAME)
                {
                    const auto &out = vstreamer::data_packet::cast<vstreamer::frame_data>(nv12);
                    if (out.kind == vstreamer::media_kind_e::NV12 && out.width == k_w &&
                        out.height == k_h)
                    {
                        ++decoded;
                    }
                }
                ir = dec.input(0, au_pkt);
            }
            while (true)
            {
                vstreamer::data_packet nv12;
                const int              orv = dec.output(0, nv12, 5);
                if (orv != 0)
                {
                    break;
                }
                if (nv12.get_type() == vstreamer::packet_kind_e::FRAME)
                {
                    const auto &out = vstreamer::data_packet::cast<vstreamer::frame_data>(nv12);
                    if (out.kind == vstreamer::media_kind_e::NV12 && out.width == k_w &&
                        out.height == k_h)
                    {
                        ++decoded;
                    }
                }
            }
        }
        EXPECT_GE(decoded, 25);
    };

    std::promise<void> done;
    auto               fut = done.get_future();
    std::thread        watchdog([&]() {
        if (fut.wait_for(std::chrono::seconds(5)) == std::future_status::timeout)
        {
            ADD_FAILURE() << "encode/decode round trip timed out (possible decoder deadlock)";
        }
    });

    run_body();
    done.set_value();
    watchdog.join();

    dec.close();
    enc.close();
}

TEST(H264DecoderMppTest, ResolutionSwitchFollowsEncoder)
{
    vstreamer::h264_encoder_mpp enc;
    vstreamer::h264_decoder_mpp dec;
    if (cfg(enc, "fps", "30") < 0 || cfg(enc, "rc", "fixqp") < 0 || cfg(enc, "qp", "36") < 0 ||
        cfg(enc, "gop", "30") < 0 || cfg(dec, "fps", "30") < 0)
    {
        GTEST_SKIP() << "configure failed";
    }
    if (enc.open() < 0 || dec.open() < 0)
    {
        enc.close();
        dec.close();
        GTEST_SKIP() << "MPP open failed";
    }

    const int segments[][2] = {{640, 480}, {1920, 1080}, {640, 480}};
    const int frames_per = 10;
    std::vector<std::pair<int, int>> decoded_sizes;

    auto drain_decoded = [&]() {
        while (true)
        {
            vstreamer::data_packet nv12;
            const int              orv = dec.output(0, nv12, 5);
            if (orv != 0)
            {
                break;
            }
            if (nv12.get_type() == vstreamer::packet_kind_e::FRAME)
            {
                const auto &out = vstreamer::data_packet::cast<vstreamer::frame_data>(nv12);
                if (out.kind == vstreamer::media_kind_e::NV12)
                {
                    decoded_sizes.emplace_back(out.width, out.height);
                }
            }
        }
    };

    for (const auto &seg : segments)
    {
        char sz[32];
        std::snprintf(sz, sizeof(sz), "%dx%d", seg[0], seg[1]);
        if (cfg(enc, "size", sz) < 0 || cfg(dec, "size", sz) < 0)
        {
            dec.close();
            enc.close();
            GTEST_SKIP() << "size configure failed";
        }
        (void)enc.configure("idr", std::string_view("1"));

        for (int frame = 0; frame < frames_per; ++frame)
        {
            const vstreamer::data_packet in = make_nv12(seg[0], seg[1], frame);
            int                          ir = enc.input(0, in);
            while (ir == -EAGAIN)
            {
                vstreamer::data_packet drain;
                (void)enc.output(0, drain, 1);
                ir = enc.input(0, in);
            }
            ASSERT_GE(ir, 0);
            for (int attempt = 0; attempt < 50; ++attempt)
            {
                vstreamer::data_packet au;
                const int              orv = enc.output(0, au, 20);
                if (orv == 0)
                {
                    int dir = dec.input(0, au);
                    for (int spin = 0; spin < 200 && dir == -EAGAIN; ++spin)
                    {
                        drain_decoded();
                        dir = dec.input(0, au);
                    }
                    break;
                }
                if (orv != -EAGAIN)
                {
                    break;
                }
            }
            drain_decoded();
        }
    }

    EXPECT_GE(decoded_sizes.size(), static_cast<size_t>(frames_per * 3 - 5));
    for (const auto &seg : segments)
    {
        const int w = seg[0];
        const int h = seg[1];
        const auto it =
            std::find_if(decoded_sizes.begin(), decoded_sizes.end(),
                         [w, h](const std::pair<int, int> &p) { return p.first == w && p.second == h; });
        EXPECT_NE(it, decoded_sizes.end()) << "missing decoded size " << w << "x" << h;
    }

    dec.close();
    enc.close();
}
