#include "components/rtp_h264_depay.hpp"
#include "components/rtp_h264_pay.hpp"
#include "core/rtp_h264.hpp"

#include <gtest/gtest.h>

#include "core/shared_sized_buffer.hpp"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{

using vstreamer::rtp_h264_config;
using vstreamer::rtp_h264_depacketizer;
using vstreamer::rtp_h264_packer;

void append_nal(std::vector<uint8_t> *au, const uint8_t *nal, size_t len)
{
    au->insert(au->end(), {0, 0, 0, 1});
    au->insert(au->end(), nal, nal + len);
}

std::vector<std::vector<uint8_t>> pack_au(const std::vector<uint8_t> &annexb, int mtu,
                                          int64_t capture_rel = 0)
{
    rtp_h264_config cfg;
    cfg.mtu = mtu;
    cfg.fps = 30;
    rtp_h264_packer packer(cfg);
    EXPECT_EQ(0, packer.pack_annexb(annexb.data(), annexb.size(), 7, capture_rel));
    std::vector<std::vector<uint8_t>> out;
    uint8_t buf[2048];
    while (packer.pending())
    {
        const int n = packer.pop_datagram(buf, sizeof(buf));
        EXPECT_GT(n, 0);
        out.emplace_back(buf, buf + n);
    }
    return out;
}

std::vector<uint8_t> depack_datagrams(rtp_h264_depacketizer &dep,
                                      const std::vector<std::vector<uint8_t>> &dgrams,
                                      int64_t epoch_ns = 0)
{
    dep.set_capture_epoch_ns(epoch_ns);
    std::vector<uint8_t> au;
    for (const auto &dg : dgrams)
    {
        std::vector<uint8_t> one;
        const int            r = dep.feed(dg.data(), dg.size(), &one);
        if (r == 1)
        {
            au = std::move(one);
        }
    }
    return au;
}

bool bytes_contain(const std::vector<uint8_t> &hay, const std::vector<uint8_t> &needle)
{
    if (needle.empty() || hay.size() < needle.size())
    {
        return false;
    }
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
    {
        if (std::equal(needle.begin(), needle.end(), hay.begin() + static_cast<std::ptrdiff_t>(i)))
        {
            return true;
        }
    }
    return false;
}

std::vector<uint8_t> make_idr_au_with_large_nal()
{
    const uint8_t sps[] = {0x67, 0x42, 0x00, 0x1f};
    const uint8_t pps[] = {0x68, 0xce, 0x38, 0x80};
    const uint8_t idr[] = {0x65, 0x88, 0x84, 0x00, 0x10};

    std::vector<uint8_t> au;
    append_nal(&au, sps, sizeof(sps));
    append_nal(&au, pps, sizeof(pps));
    append_nal(&au, idr, sizeof(idr));

    std::vector<uint8_t> large;
    large.push_back(0x41);
    large.insert(large.end(), 1200, 0xAB);
    append_nal(&au, large.data(), large.size());
    return au;
}

}  // namespace

TEST(RtpH264Test, RoundTripIdrAuLargeNal)
{
    const std::vector<uint8_t> src = make_idr_au_with_large_nal();
    const auto                 dgrams = pack_au(src, 200);
    EXPECT_GT(dgrams.size(), 3U);

    rtp_h264_depacketizer dep(30);
    const std::vector<uint8_t> out = depack_datagrams(dep, dgrams);
    EXPECT_EQ(src, out);
    EXPECT_TRUE(dep.au_key());
}

TEST(RtpH264Test, FuMiddleFragmentDropped)
{
    const std::vector<uint8_t> src = make_idr_au_with_large_nal();
    auto                       dgrams = pack_au(src, 200);
    ASSERT_GT(dgrams.size(), 4U);

    size_t fu_idx = 0;
    for (size_t i = 0; i < dgrams.size(); ++i)
    {
        if (dgrams[i].size() > 12 && (dgrams[i][12] & 0x1f) == 28)
        {
            fu_idx = i;
            break;
        }
    }
    ASSERT_GT(fu_idx, 0U);
    if (fu_idx + 1 < dgrams.size())
    {
        dgrams.erase(dgrams.begin() + static_cast<std::ptrdiff_t>(fu_idx + 1));
    }

    const uint64_t need_before = 0;
    rtp_h264_depacketizer dep(30);
    const std::vector<uint8_t> out = depack_datagrams(dep, dgrams);
    EXPECT_NE(src, out);
    EXPECT_GT(dep.need_idr(), need_before);
    EXPECT_GT(dep.nal_dropped(), 0U);
    EXPECT_TRUE(bytes_contain(out, {0x00, 0x00, 0x00, 0x01, 0x65}));
}

TEST(RtpH264Test, ReorderDoesNotInflatePacketLoss)
{
    const std::vector<uint8_t> src = make_idr_au_with_large_nal();
    auto                       dgrams = pack_au(src, 1400);
    ASSERT_GE(dgrams.size(), 2U);
    std::swap(dgrams[0], dgrams[1]);

    rtp_h264_depacketizer dep(30);
    for (const auto &dg : dgrams)
    {
        std::vector<uint8_t> au;
        (void)dep.feed(dg.data(), dg.size(), &au);
    }
    EXPECT_EQ(0.f, dep.packet_loss());
    EXPECT_GE(dep.rtp_reordered(), 1U);
}

TEST(RtpH264Test, CsrcAndPaddingParsed)
{
    const uint8_t nal[] = {0x65, 0x01, 0x02};
    std::vector<uint8_t> pkt = {
        0xa2, 0xe0, 0x00, 0x01, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x01,
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11,
        nal[0], nal[1], nal[2], 0x00, 0x00, 0x00, 0x04,
    };

    rtp_h264_depacketizer dep(30);
    std::vector<uint8_t>  au;
    EXPECT_EQ(1, dep.feed(pkt.data(), pkt.size(), &au));
    EXPECT_EQ(std::vector<uint8_t>({0, 0, 0, 1, nal[0], nal[1], nal[2]}), au);
}

TEST(RtpH264Test, StapATwoNals)
{
    const uint8_t n1[] = {0x67, 0x42};
    const uint8_t n2[] = {0x68, 0xce};
    std::vector<uint8_t> payload;
    payload.push_back(0x78);
    payload.push_back(static_cast<uint8_t>((sizeof(n1) >> 8) & 0xff));
    payload.push_back(static_cast<uint8_t>(sizeof(n1) & 0xff));
    payload.insert(payload.end(), n1, n1 + sizeof(n1));
    payload.push_back(static_cast<uint8_t>((sizeof(n2) >> 8) & 0xff));
    payload.push_back(static_cast<uint8_t>(sizeof(n2) & 0xff));
    payload.insert(payload.end(), n2, n2 + sizeof(n2));

    std::vector<uint8_t> pkt(12 + payload.size());
    pkt[0] = 0x80;
    pkt[1] = 0x80;
    pkt[2] = 0;
    pkt[3] = 1;
    std::memcpy(pkt.data() + 12, payload.data(), payload.size());

    rtp_h264_depacketizer dep(30);
    std::vector<uint8_t>  au;
    EXPECT_EQ(1, dep.feed(pkt.data(), pkt.size(), &au));
    EXPECT_TRUE(bytes_contain(au, {0, 0, 0, 1, 0x67}));
    EXPECT_TRUE(bytes_contain(au, {0, 0, 0, 1, 0x68}));
}

TEST(RtpH264Test, SpsPpsOnlyBeforeIdrWithoutInBandParams)
{
    const uint8_t sps[] = {0x67, 0x01};
    const uint8_t pps[] = {0x68, 0x02};
    const uint8_t idr[] = {0x65, 0x03};

    rtp_h264_config cfg;
    cfg.mtu = 1400;
    rtp_h264_packer packer(cfg);
    std::vector<uint8_t> prime;
    append_nal(&prime, sps, sizeof(sps));
    append_nal(&prime, pps, sizeof(pps));
    ASSERT_EQ(0, packer.pack_annexb(prime.data(), prime.size(), 0, 0));
    while (packer.pending())
    {
        uint8_t scratch[2048];
        (void)packer.pop_datagram(scratch, sizeof(scratch));
    }

    std::vector<uint8_t> idr_only;
    append_nal(&idr_only, idr, sizeof(idr));
    ASSERT_EQ(0, packer.pack_annexb(idr_only.data(), idr_only.size(), 1, 0));
    std::vector<std::vector<uint8_t>> dgrams;
    uint8_t buf[2048];
    while (packer.pending())
    {
        const int n = packer.pop_datagram(buf, sizeof(buf));
        ASSERT_GT(n, 0);
        dgrams.emplace_back(buf, buf + n);
    }

    int param_count = 0;
    for (const auto &dg : dgrams)
    {
        if (dg.size() < 13)
        {
            continue;
        }
        const uint8_t t = dg[12] & 0x1f;
        if (7 == t || 8 == t)
        {
            param_count++;
        }
    }
    EXPECT_EQ(2, param_count);
}

TEST(RtpH264Test, NonIdrAuDoesNotEmitExtraParams)
{
    const uint8_t sps[] = {0x67, 0x01};
    const uint8_t pps[] = {0x68, 0x02};
    const uint8_t p[] = {0x41, 0x09};

    rtp_h264_config cfg;
    cfg.mtu = 1400;
    rtp_h264_packer packer(cfg);
    std::vector<uint8_t> prime;
    append_nal(&prime, sps, sizeof(sps));
    append_nal(&prime, pps, sizeof(pps));
    ASSERT_EQ(0, packer.pack_annexb(prime.data(), prime.size(), 0, 0));
    while (packer.pending())
    {
        uint8_t scratch[2048];
        (void)packer.pop_datagram(scratch, sizeof(scratch));
    }

    std::vector<uint8_t> p_au;
    append_nal(&p_au, p, sizeof(p));
    ASSERT_EQ(0, packer.pack_annexb(p_au.data(), p_au.size(), 1, 0));
    std::vector<std::vector<uint8_t>> dgrams;
    uint8_t buf[2048];
    while (packer.pending())
    {
        const int n = packer.pop_datagram(buf, sizeof(buf));
        ASSERT_GT(n, 0);
        dgrams.emplace_back(buf, buf + n);
    }

    int param_count = 0;
    for (const auto &dg : dgrams)
    {
        if (dg.size() < 13)
        {
            continue;
        }
        const uint8_t t = dg[12] & 0x1f;
        if (7 == t || 8 == t)
        {
            param_count++;
        }
    }
    EXPECT_EQ(0, param_count);
}

TEST(RtpH264Test, IdrWithInBandParamsNotDuplicated)
{
    const uint8_t sps[] = {0x67, 0x01};
    const uint8_t pps[] = {0x68, 0x02};
    const uint8_t idr[] = {0x65, 0x03};

    std::vector<uint8_t> au;
    append_nal(&au, sps, sizeof(sps));
    append_nal(&au, pps, sizeof(pps));
    append_nal(&au, idr, sizeof(idr));
    const auto dgrams = pack_au(au, 1400);

    int sps_count = 0;
    int pps_count = 0;
    for (const auto &dg : dgrams)
    {
        if (dg.size() < 13)
        {
            continue;
        }
        const uint8_t t = dg[12] & 0x1f;
        if (7 == t)
        {
            sps_count++;
        }
        if (8 == t)
        {
            pps_count++;
        }
    }
    EXPECT_EQ(1, sps_count);
    EXPECT_EQ(1, pps_count);
}

TEST(RtpH264Test, PackAuWith100Nals)
{
    std::vector<uint8_t> au;
    for (int i = 0; i < 100; ++i)
    {
        const uint8_t nal[] = {0x41, static_cast<uint8_t>(i & 0xff)};
        append_nal(&au, nal, sizeof(nal));
    }
    const auto dgrams = pack_au(au, 1400);
    EXPECT_GT(dgrams.size(), 0U);

    rtp_h264_depacketizer dep(30);
    const std::vector<uint8_t> out = depack_datagrams(dep, dgrams);
    EXPECT_EQ(au, out);
}

TEST(RtpH264Test, PayRetainsUnpulledDatagrams)
{
    vstreamer::rtp_h264_pay pay;
    ASSERT_EQ(0, pay.open());

    const uint8_t idr[] = {0x65, 0x01, 0x02, 0x03};
    std::vector<uint8_t> au;
    append_nal(&au, idr, sizeof(idr));

    auto fd = std::make_unique<vstreamer::frame_data>();
    fd->kind = vstreamer::media_kind_e::H264;
    fd->pts = 1;
    fd->buf = vstreamer::shared_sized_buffer::copy_from(au.data(), au.size());
    vstreamer::data_packet pkt;
    pkt.reset(std::move(fd));

    ASSERT_EQ(0, pay.input(0, pkt));
    vstreamer::data_packet out1;
    EXPECT_EQ(0, pay.output(0, out1, 0));

    const uint8_t p[] = {0x41, 0x04};
    std::vector<uint8_t> au2;
    append_nal(&au2, p, sizeof(p));
    auto fd2 = std::make_unique<vstreamer::frame_data>();
    fd2->kind = vstreamer::media_kind_e::H264;
    fd2->pts = 2;
    fd2->buf = vstreamer::shared_sized_buffer::copy_from(au2.data(), au2.size());
    vstreamer::data_packet pkt2;
    pkt2.reset(std::move(fd2));
    ASSERT_EQ(0, pay.input(0, pkt2));

    vstreamer::data_packet out2;
    EXPECT_EQ(0, pay.output(0, out2, 0));
    EXPECT_NE(0, pay.output(0, out2, 0));
    pay.close();
}

TEST(RtpH264Test, PayMtuRtpLimitsOnly)
{
    vstreamer::rtp_h264_pay pay;
    EXPECT_LT(pay.configure("mtu", "20"), 0);
    EXPECT_EQ(0, pay.configure("mtu", "9000"));
    EXPECT_EQ(0, pay.open());
    const uint8_t idr[] = {0x65, 0x01};
    std::vector<uint8_t> au;
    append_nal(&au, idr, sizeof(idr));
    auto fd = std::make_shared<vstreamer::frame_data>();
    fd->kind = vstreamer::media_kind_e::H264;
    fd->pts = 1;
    fd->buf = vstreamer::shared_sized_buffer::copy_from(au.data(), au.size());
    vstreamer::data_packet pkt;
    pkt.reset(std::move(fd));
    EXPECT_EQ(0, pay.input(0, pkt));
    pay.close();
}

TEST(RtpH264Test, CaptureEpochRoundTrip)
{
    constexpr int64_t k_epoch = 1'000'000'000'000LL;
    constexpr int64_t k_cap = k_epoch + 42'000'000LL;
    const int64_t     rel = k_cap - k_epoch;

    const uint8_t idr[] = {0x65, 0x01};
    std::vector<uint8_t> au;
    append_nal(&au, idr, sizeof(idr));
    const auto dgrams = pack_au(au, 1400, rel);

    rtp_h264_depacketizer dep(30);
    const std::vector<uint8_t> out = depack_datagrams(dep, dgrams, k_epoch);
    EXPECT_FALSE(out.empty());
    EXPECT_EQ(k_cap, dep.au_capture_mono_ns());
}

TEST(RtpH264Test, CaptureZeroWithoutEpoch)
{
    const uint8_t idr[] = {0x65, 0x01};
    std::vector<uint8_t> au;
    append_nal(&au, idr, sizeof(idr));
    const auto dgrams = pack_au(au, 1400, 12345);

    rtp_h264_depacketizer dep(30);
    (void)depack_datagrams(dep, dgrams, 0);
    EXPECT_EQ(0, dep.au_capture_mono_ns());
}

TEST(RtpH264Test, DepayComponentQueuesMultipleAus)
{
    vstreamer::rtp_h264_depay depay;
    ASSERT_EQ(0, depay.configure("fps", "30"));
    ASSERT_EQ(0, depay.open());

    const uint8_t idr[] = {0x65, 0x01};
    std::vector<uint8_t> au;
    append_nal(&au, idr, sizeof(idr));
    const auto dgrams = pack_au(au, 1400);

    for (const auto &dg : dgrams)
    {
        auto sd = std::make_unique<vstreamer::sock_data>();
        sd->buf = vstreamer::shared_sized_buffer::copy_from(dg.data(), dg.size());
        vstreamer::data_packet in;
        in.reset(std::move(sd));
        ASSERT_EQ(0, depay.input(0, in));
    }

    vstreamer::data_packet out;
    EXPECT_EQ(0, depay.output(0, out, 0));
    depay.close();
}

std::vector<int> annexb_nal_types(const std::vector<uint8_t> &au)
{
    std::vector<int> types;
    for (size_t i = 0; i + 4 < au.size(); ++i)
    {
        if (au[i] == 0 && au[i + 1] == 0 && au[i + 2] == 0 && au[i + 3] == 1)
        {
            const size_t off = i + 4;
            if (off < au.size())
            {
                types.push_back(static_cast<int>(au[off] & 0x1F));
            }
        }
    }
    return types;
}

std::vector<uint8_t> depack_packer_au(rtp_h264_packer &packer, const std::vector<uint8_t> &annexb)
{
    EXPECT_EQ(0, packer.pack_annexb(annexb.data(), annexb.size(), 1, 0));
    std::vector<std::vector<uint8_t>> dgrams;
    uint8_t                           buf[2048];
    while (packer.pending())
    {
        const int n = packer.pop_datagram(buf, sizeof(buf));
        EXPECT_GT(n, 0);
        dgrams.emplace_back(buf, buf + n);
    }
    rtp_h264_depacketizer dep(30);
    return depack_datagrams(dep, dgrams);
}

TEST(RtpH264Test, InjectCachedPpsBeforeIdrWhenSpsPresent)
{
    const uint8_t sps[] = {0x67, 0x42, 0x00, 0x1f};
    const uint8_t pps[] = {0x68, 0xce, 0x38, 0x80};
    const uint8_t idr[] = {0x65, 0x88, 0x84, 0x00, 0x10};

    rtp_h264_config cfg;
    cfg.mtu = 1400;
    cfg.fps = 30;
    rtp_h264_packer packer(cfg);

    std::vector<uint8_t> prime;
    append_nal(&prime, sps, sizeof(sps));
    append_nal(&prime, pps, sizeof(pps));
    append_nal(&prime, idr, sizeof(idr));
    (void)depack_packer_au(packer, prime);

    std::vector<uint8_t> au;
    append_nal(&au, sps, sizeof(sps));
    append_nal(&au, idr, sizeof(idr));
    const std::vector<uint8_t> out = depack_packer_au(packer, au);
    EXPECT_EQ(std::vector<int>({7, 8, 5}), annexb_nal_types(out));
}

TEST(RtpH264Test, InjectCachedParamsBeforeIdrOnly)
{
    const uint8_t sps[] = {0x67, 0x42, 0x00, 0x1f};
    const uint8_t pps[] = {0x68, 0xce, 0x38, 0x80};
    const uint8_t idr[] = {0x65, 0x88, 0x84, 0x00, 0x10};

    rtp_h264_config cfg;
    cfg.mtu = 1400;
    cfg.fps = 30;
    rtp_h264_packer packer(cfg);

    std::vector<uint8_t> prime;
    append_nal(&prime, sps, sizeof(sps));
    append_nal(&prime, pps, sizeof(pps));
    append_nal(&prime, idr, sizeof(idr));
    (void)depack_packer_au(packer, prime);

    std::vector<uint8_t> au;
    append_nal(&au, idr, sizeof(idr));
    const std::vector<uint8_t> out = depack_packer_au(packer, au);
    EXPECT_EQ(std::vector<int>({7, 8, 5}), annexb_nal_types(out));
}

TEST(RtpH264Test, NoInjectWhenSpsAndPpsPresent)
{
    const uint8_t sps[] = {0x67, 0x42, 0x00, 0x1f};
    const uint8_t pps[] = {0x68, 0xce, 0x38, 0x80};
    const uint8_t idr[] = {0x65, 0x88, 0x84, 0x00, 0x10};

    rtp_h264_config cfg;
    cfg.mtu = 1400;
    cfg.fps = 30;
    rtp_h264_packer packer(cfg);

    std::vector<uint8_t> au;
    append_nal(&au, sps, sizeof(sps));
    append_nal(&au, pps, sizeof(pps));
    append_nal(&au, idr, sizeof(idr));
    const std::vector<uint8_t> out = depack_packer_au(packer, au);
    EXPECT_EQ(std::vector<int>({7, 8, 5}), annexb_nal_types(out));
}

TEST(RtpH264Test, NoInjectOnNonIdrSlice)
{
    const uint8_t sps[] = {0x67, 0x42, 0x00, 0x1f};
    const uint8_t pps[] = {0x68, 0xce, 0x38, 0x80};
    const uint8_t idr[] = {0x65, 0x88, 0x84, 0x00, 0x10};
    const uint8_t slice[] = {0x41, 0x9a, 0x24};

    rtp_h264_config cfg;
    cfg.mtu = 1400;
    cfg.fps = 30;
    rtp_h264_packer packer(cfg);

    std::vector<uint8_t> prime;
    append_nal(&prime, sps, sizeof(sps));
    append_nal(&prime, pps, sizeof(pps));
    append_nal(&prime, idr, sizeof(idr));
    (void)depack_packer_au(packer, prime);

    std::vector<uint8_t> au;
    append_nal(&au, slice, sizeof(slice));
    const std::vector<uint8_t> out = depack_packer_au(packer, au);
    EXPECT_EQ(std::vector<int>({1}), annexb_nal_types(out));
}
