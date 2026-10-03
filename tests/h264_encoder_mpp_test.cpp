#include "components/h264_encoder_mpp.hpp"

#include "core/data_packet.hpp"
#include "core/packet_types.hpp"
#include "core/shared_sized_buffer.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstring>
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

TEST(H264EncoderMppTest, ConcurrentInputOutputConfigure)
{
    vstreamer::h264_encoder_mpp enc;
    if (cfg(enc, "size", "320x240") < 0 || cfg(enc, "fps", "30") < 0 || cfg(enc, "rc", "fixqp") < 0 ||
        cfg(enc, "qp", "36") < 0 || cfg(enc, "gop", "30") < 0)
    {
        GTEST_SKIP() << "configure failed";
    }
    if (enc.open() < 0)
    {
        GTEST_SKIP() << "MPP encoder open failed (no hardware?)";
    }

    std::atomic<bool> run {true};
    constexpr int     k_w = 320;
    constexpr int     k_h = 240;

    std::thread input_thr([&] {
        int64_t pts = 0;
        while (run.load(std::memory_order_relaxed))
        {
            const vstreamer::data_packet pkt = make_nv12(k_w, k_h, pts++);
            const int                    ir = enc.input(0, pkt);
            if (ir < 0 && ir != -EAGAIN)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    std::thread output_thr([&] {
        while (run.load(std::memory_order_relaxed))
        {
            vstreamer::data_packet out;
            (void)enc.output(0, out, 5);
        }
    });

    std::thread cfg_thr([&] {
        const char *keys[] = {"qp", "gop", "cbr", "rc"};
        const char *vals[] = {"32", "15", "2000000", "cbr"};
        int         i = 0;
        while (run.load(std::memory_order_relaxed))
        {
            (void)cfg(enc, keys[i % 4], vals[i % 4]);
            if (0 == (i % 4))
            {
                (void)cfg(enc, "rc", "fixqp");
            }
            ++i;
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(5));
    run.store(false, std::memory_order_relaxed);
    input_thr.join();
    output_thr.join();
    cfg_thr.join();
    enc.close();
}
