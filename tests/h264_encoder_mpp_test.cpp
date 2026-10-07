#include "components/h264_encoder_mpp.hpp"

#include "core/shared_sized_buffer.hpp"

#include <gtest/gtest.h>

#include "test_pdu_helpers.hpp"

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
    ASSERT_EQ(0, enc.input(vstreamer::test_pdu::make_nv12_caps(k_w, k_h, 30)));

    std::thread input_thr([&] {
        uint64_t ts_us = 0;
        while (run.load(std::memory_order_relaxed))
        {
            vstreamer::component_pdu pkt = vstreamer::test_pdu::make_nv12(k_w, k_h, ts_us++);
            const int                ir = enc.input(std::move(pkt));
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
            vstreamer::component_pdu out;
            (void)enc.output(out);
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
