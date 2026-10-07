#include "components/h264_decoder_mpp.hpp"
#include "components/h264_encoder_mpp.hpp"

#include <gtest/gtest.h>

#include "test_pdu_helpers.hpp"

#include <chrono>
#include <cerrno>
#include <thread>
#include <vector>

namespace
{

int cfg(vstreamer::component &c, const char *key, const char *val)
{
    return c.configure(key, val);
}

}  // namespace

TEST(H264DecoderMppTest, EncodeDecodeRoundTrip)
{
    vstreamer::h264_encoder_mpp enc;
    vstreamer::h264_decoder_mpp dec;
    if (cfg(enc, "size", "320x240") < 0 || cfg(enc, "fps", "30") < 0 || cfg(enc, "rc", "fixqp") < 0 ||
        cfg(enc, "qp", "36") < 0 || cfg(dec, "size", "320x240") < 0)
    {
        GTEST_SKIP() << "configure failed";
    }
    if (enc.open() < 0 || dec.open() < 0)
    {
        GTEST_SKIP() << "MPP codec open failed (no hardware?)";
    }

    constexpr int k_w = 320;
    constexpr int k_h = 240;
    ASSERT_EQ(0, enc.input(vstreamer::test_pdu::make_nv12_caps(k_w, k_h, 30)));

    vstreamer::component_pdu nv12 = vstreamer::test_pdu::make_nv12(k_w, k_h, 1);
    ASSERT_EQ(0, enc.input(std::move(nv12)));

    vstreamer::component_pdu au;
    for (int i = 0; i < 200; ++i)
    {
        if (0 == enc.output(au))
        {
            break;
        }
    }
    if (au.sdu_type != vstreamer::sdu_type_e::H264_AU)
    {
        GTEST_SKIP() << "no encoded AU from MPP encoder";
    }

    ASSERT_EQ(0, dec.input(vstreamer::test_pdu::make_h264_coded_caps(k_w, k_h, 30)));
    ASSERT_EQ(0, dec.input(std::move(au)));

    vstreamer::component_pdu out;
    for (int i = 0; i < 200; ++i)
    {
        if (0 == dec.output(out))
        {
            if (out.sdu_type == vstreamer::sdu_type_e::NV12)
            {
                EXPECT_GE(out.sdu.size(), static_cast<size_t>(k_w * k_h * 3 / 2));
                enc.close();
                dec.close();
                return;
            }
        }
    }
    GTEST_SKIP() << "decoder produced no NV12";
}

namespace
{

bool encode_one_au(vstreamer::h264_encoder_mpp &enc, int w, int h, uint64_t seq,
                   vstreamer::component_pdu *au_out)
{
    vstreamer::component_pdu nv12 = vstreamer::test_pdu::make_nv12(w, h, seq);
    if (0 != enc.input(std::move(nv12)))
    {
        return false;
    }
    for (int i = 0; i < 200; ++i)
    {
        vstreamer::component_pdu au;
        if (0 == enc.output(au) && au.sdu_type == vstreamer::sdu_type_e::H264_AU)
        {
            *au_out = std::move(au);
            return true;
        }
    }
    return false;
}

int drain_nv12(vstreamer::h264_decoder_mpp &dec, vstreamer::component_pdu *out)
{
    for (int i = 0; i < 400; ++i)
    {
        vstreamer::component_pdu pdu;
        const int r = dec.output(pdu);
        if (0 == r && pdu.sdu_type == vstreamer::sdu_type_e::NV12)
        {
            *out = std::move(pdu);
            return 0;
        }
        if (0 == r && pdu.sdu_type == vstreamer::sdu_type_e::CAPS_VIDEO_RAW)
        {
            continue;
        }
        if (-EAGAIN == r)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        return r;
    }
    return -ETIMEDOUT;
}

}  // namespace

TEST(H264DecoderMppTest, DrainThreadFlushesWithoutFurtherInput)
{
    vstreamer::h264_encoder_mpp enc;
    vstreamer::h264_decoder_mpp dec;
    constexpr int k_w = 320;
    constexpr int k_h = 240;
    if (cfg(enc, "size", "320x240") < 0 || cfg(enc, "fps", "30") < 0 || cfg(enc, "rc", "fixqp") < 0 ||
        cfg(enc, "qp", "36") < 0 || cfg(enc, "gop", "1") < 0 || cfg(dec, "size", "320x240") < 0)
    {
        GTEST_SKIP() << "configure failed";
    }
    if (enc.open() < 0 || dec.open() < 0)
    {
        GTEST_SKIP() << "MPP codec open failed (no hardware?)";
    }
    ASSERT_EQ(0, enc.input(vstreamer::test_pdu::make_nv12_caps(k_w, k_h, 30)));
    ASSERT_EQ(0, dec.input(vstreamer::test_pdu::make_h264_coded_caps(k_w, k_h, 30)));

    for (uint64_t seq = 1; seq <= 8; ++seq)
    {
        vstreamer::component_pdu au;
        ASSERT_TRUE(encode_one_au(enc, k_w, k_h, seq, &au));
        ASSERT_EQ(0, dec.input(std::move(au)));
        vstreamer::component_pdu nv12;
        if (0 == drain_nv12(dec, &nv12))
        {
            continue;
        }
    }

    constexpr int k_fed = 5;
    for (uint64_t seq = 100; seq < 100 + k_fed; ++seq)
    {
        vstreamer::component_pdu au;
        ASSERT_TRUE(encode_one_au(enc, k_w, k_h, seq, &au));
        ASSERT_EQ(0, dec.input(std::move(au)));
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    int got = 0;
    while (std::chrono::steady_clock::now() < deadline)
    {
        vstreamer::component_pdu out;
        const int r = dec.output(out);
        if (0 == r && out.sdu_type == vstreamer::sdu_type_e::NV12)
        {
            ++got;
            if (got >= k_fed)
            {
                break;
            }
            continue;
        }
        if (0 == r && out.sdu_type == vstreamer::sdu_type_e::CAPS_VIDEO_RAW)
        {
            continue;
        }
        if (-EAGAIN == r)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        break;
    }
    EXPECT_EQ(k_fed, got) << "all fed AUs must appear without another input()";
    enc.close();
    dec.close();
}

TEST(H264DecoderMppTest, RecoversWhenReadyQueueFull)
{
    vstreamer::h264_encoder_mpp enc;
    vstreamer::h264_decoder_mpp dec;
    constexpr int k_w = 320;
    constexpr int k_h = 240;
    if (cfg(enc, "size", "320x240") < 0 || cfg(enc, "fps", "30") < 0 || cfg(enc, "rc", "fixqp") < 0 ||
        cfg(enc, "qp", "36") < 0 || cfg(dec, "size", "320x240") < 0)
    {
        GTEST_SKIP() << "configure failed";
    }
    if (enc.open() < 0 || dec.open() < 0)
    {
        GTEST_SKIP() << "MPP codec open failed (no hardware?)";
    }
    ASSERT_EQ(0, enc.input(vstreamer::test_pdu::make_nv12_caps(k_w, k_h, 30)));
    ASSERT_EQ(0, dec.input(vstreamer::test_pdu::make_h264_coded_caps(k_w, k_h, 30)));

    int fed = 0;
    bool saw_eagain = false;
    for (uint64_t seq = 1; seq <= 24; ++seq)
    {
        vstreamer::component_pdu au;
        if (!encode_one_au(enc, k_w, k_h, seq, &au))
        {
            break;
        }
        int r = dec.input(std::move(au));
        if (-EAGAIN == r)
        {
            saw_eagain = true;
            vstreamer::component_pdu nv12;
            ASSERT_EQ(0, drain_nv12(dec, &nv12));
            --seq;
            continue;
        }
        ASSERT_EQ(0, r);
        ++fed;
    }
    EXPECT_TRUE(saw_eagain);
    EXPECT_GE(fed, 12);
    enc.close();
    dec.close();
}
