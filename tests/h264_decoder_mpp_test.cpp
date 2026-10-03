#include "components/h264_decoder_mpp.hpp"
#include "components/h264_encoder_mpp.hpp"

#include "core/data_packet.hpp"
#include "core/packet_types.hpp"
#include "core/shared_sized_buffer.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <future>
#include <thread>
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
