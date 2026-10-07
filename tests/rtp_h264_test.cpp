#include "components/rtp_h264_depay.hpp"
#include "components/rtp_h264_pay.hpp"
#include "core/rtp_h264.hpp"

#include <gtest/gtest.h>

#include "test_pdu_helpers.hpp"

#include "core/shared_sized_buffer.hpp"
#include "core/time_util.hpp"

#include <cstdint>
#include <cstdlib>
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
                                          int64_t capture_rt = 0)
{
    rtp_h264_config cfg;
    cfg.mtu = mtu;
    cfg.fps = 30;
    rtp_h264_packer packer(cfg);
    EXPECT_EQ(0, packer.pack_annexb(annexb.data(), annexb.size(), 7, capture_rt));
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
                                      const std::vector<std::vector<uint8_t>> &dgrams)
{
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

TEST(RtpH264Test, PayLargeIdrThenAcceptsNextAu)
{
    vstreamer::rtp_h264_pay pay;
    ASSERT_EQ(0, pay.open());
    ASSERT_EQ(0, pay.input(vstreamer::test_pdu::make_h264_coded_caps(64, 64)));

    std::vector<uint8_t> idr_nal;
    idr_nal.push_back(0x65);
    idr_nal.insert(idr_nal.end(), 1024 * 1024, 0xAB);
    std::vector<uint8_t> au;
    append_nal(&au, idr_nal.data(), idr_nal.size());
    ASSERT_EQ(0, pay.input(vstreamer::test_pdu::make_h264_au(au.data(), au.size(), 1)));

    size_t drained = 0;
    vstreamer::component_pdu out;
    while (pay.output(out) == 0)
    {
        ++drained;
    }
    EXPECT_GT(drained, 512U);

    const uint8_t p[] = {0x41, 0x04};
    std::vector<uint8_t> au2;
    append_nal(&au2, p, sizeof(p));
    EXPECT_EQ(0, pay.input(vstreamer::test_pdu::make_h264_au(au2.data(), au2.size(), 2, false)));
    pay.close();
}

TEST(RtpH264Test, PayRetainsUnpulledDatagrams)
{
    vstreamer::rtp_h264_pay pay;
    ASSERT_EQ(0, pay.open());
    ASSERT_EQ(0, pay.input(vstreamer::test_pdu::make_h264_coded_caps(64, 64)));

    const uint8_t idr[] = {0x65, 0x01, 0x02, 0x03};
    std::vector<uint8_t> au;
    append_nal(&au, idr, sizeof(idr));
    ASSERT_EQ(0, pay.input(vstreamer::test_pdu::make_h264_au(au.data(), au.size(), 1)));

    vstreamer::component_pdu out1;
    EXPECT_EQ(0, pay.output(out1));

    const uint8_t p[] = {0x41, 0x04};
    std::vector<uint8_t> au2;
    append_nal(&au2, p, sizeof(p));
    ASSERT_EQ(0, pay.input(vstreamer::test_pdu::make_h264_au(au2.data(), au2.size(), 2, false)));

    vstreamer::component_pdu out2;
    EXPECT_EQ(0, pay.output(out2));
    EXPECT_EQ(-EAGAIN, pay.output(out2));
    pay.close();
}

TEST(RtpH264Test, PayMtuRtpLimitsOnly)
{
    vstreamer::rtp_h264_pay pay;
    EXPECT_LT(pay.configure("mtu", "20"), 0);
    EXPECT_EQ(0, pay.configure("mtu", "9000"));
    EXPECT_EQ(0, pay.open());
    ASSERT_EQ(0, pay.input(vstreamer::test_pdu::make_h264_coded_caps(64, 64)));
    const uint8_t idr[] = {0x65, 0x01};
    std::vector<uint8_t> au;
    append_nal(&au, idr, sizeof(idr));
    EXPECT_EQ(0, pay.input(vstreamer::test_pdu::make_h264_au(au.data(), au.size(), 1)));
    pay.close();
}

TEST(RtpH264Test, CaptureExtensionGoldenHeader)
{
    constexpr int64_t k_rt = 1'704'067'200'000'000'000LL;
    const uint8_t     idr[] = {0x65, 0x01};
    std::vector<uint8_t> au;
    append_nal(&au, idr, sizeof(idr));
    const auto dgrams = pack_au(au, 1400, k_rt);
    ASSERT_FALSE(dgrams.empty());
    const auto &dg = dgrams.front();
    EXPECT_GE(dg.size(), 28U);
    EXPECT_EQ(0xbe, dg[12]);
    EXPECT_EQ(0xde, dg[13]);
    EXPECT_EQ(0x00, dg[14]);
    EXPECT_EQ(0x03, dg[15]);
    EXPECT_EQ(0x27, dg[16]);
    uint64_t be = 0;
    for (int i = 0; i < 8; ++i)
    {
        be = (be << 8) | dg[17 + i];
    }
    EXPECT_EQ(k_rt, static_cast<int64_t>(be));
}

TEST(RtpH264Test, CaptureRealtimeRoundTrip)
{
    constexpr int64_t k_rt = 1'704'067'200'042'000'000LL;

    const uint8_t idr[] = {0x65, 0x01};
    std::vector<uint8_t> au;
    append_nal(&au, idr, sizeof(idr));
    const auto dgrams = pack_au(au, 1400, k_rt);

    rtp_h264_depacketizer dep(30);
    const std::vector<uint8_t> out = depack_datagrams(dep, dgrams);
    EXPECT_FALSE(out.empty());
    EXPECT_EQ(k_rt, dep.au_capture_rt_ns());
}

TEST(RtpH264Test, OldCaptureExtensionIdIgnored)
{
    const uint8_t idr[] = {0x65, 0x01};
    std::vector<uint8_t> au;
    append_nal(&au, idr, sizeof(idr));
    auto dgrams = pack_au(au, 1400, vstreamer::realtime_ns());
    ASSERT_FALSE(dgrams.empty());
    dgrams.front()[16] = 0x17;

    rtp_h264_depacketizer dep(30);
    (void)depack_datagrams(dep, dgrams);
    EXPECT_EQ(0, dep.au_capture_rt_ns());
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
        vstreamer::component_pdu in;
        in.sdu_type = vstreamer::sdu_type_e::RTP;
        in.port = 0;
        in.sdu = vstreamer::shared_sized_buffer::copy_from(dg.data(), dg.size());
        ASSERT_EQ(0, depay.input(std::move(in)));
    }

    vstreamer::component_pdu out;
    int                   drained = 0;
    while (0 == depay.output(out))
    {
        ++drained;
    }
    EXPECT_GT(drained, 0);
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

void feed_depay_rtp(vstreamer::rtp_h264_depay &depay, const std::vector<uint8_t> &dg)
{
    vstreamer::component_pdu in;
    in.sdu_type = vstreamer::sdu_type_e::RTP;
    in.port = 0;
    in.sdu = vstreamer::shared_sized_buffer::copy_from(dg.data(), dg.size());
    EXPECT_EQ(0, depay.input(std::move(in)));
}

int64_t depay_output_capture_mono(vstreamer::rtp_h264_depay &depay)
{
    for (;;)
    {
        vstreamer::component_pdu out;
        EXPECT_EQ(0, depay.output(out));
        if (out.sdu_type == vstreamer::sdu_type_e::H264_AU)
        {
            return static_cast<int64_t>(out.ts_us) * 1000LL;
        }
    }
}

uint64_t depay_query_u64(vstreamer::rtp_h264_depay &depay, const char *key)
{
    std::string v;
    EXPECT_EQ(0, depay.query(key, &v));
    return static_cast<uint64_t>(std::strtoull(v.c_str(), nullptr, 10));
}

TEST(RtpH264Test, PayDepayCaptureMonoRoundTrip)
{
    vstreamer::rtp_h264_pay pay;
    vstreamer::rtp_h264_depay depay;
    ASSERT_EQ(0, pay.open());
    ASSERT_EQ(0, depay.open());

    const int64_t cap_mono = vstreamer::steady_mono_ns() - 30'000'000LL;
    const uint8_t idr[] = {0x65, 0x01};
    std::vector<uint8_t> au;
    append_nal(&au, idr, sizeof(idr));

    ASSERT_EQ(0, pay.input(vstreamer::test_pdu::make_h264_coded_caps(64, 64)));
    const uint64_t ts_us = static_cast<uint64_t>(cap_mono / 1000LL);
    ASSERT_EQ(0, pay.input(vstreamer::test_pdu::make_h264_au(au.data(), au.size(), ts_us)));

    vstreamer::component_pdu rtp_pdu;
    ASSERT_EQ(0, pay.output(rtp_pdu));
    const std::vector<uint8_t> dg(rtp_pdu.sdu.u8(), rtp_pdu.sdu.u8() + rtp_pdu.sdu.size());
    feed_depay_rtp(depay, dg);

    const int64_t out_cap = depay_output_capture_mono(depay);
    EXPECT_GT(out_cap, 0);
    const int64_t delta = out_cap - cap_mono;
    EXPECT_GE(delta, -5'000'000LL);
    EXPECT_LE(delta, 5'000'000LL);
    pay.close();
    depay.close();
}

TEST(RtpH264Test, DepayRejectsCaptureTooFarAhead)
{
    vstreamer::rtp_h264_depay depay;
    ASSERT_EQ(0, depay.open());

    const uint8_t idr[] = {0x65, 0x01};
    std::vector<uint8_t> au;
    append_nal(&au, idr, sizeof(idr));
    const int64_t future_rt = vstreamer::realtime_ns() + 200'000'000LL;
    const auto    dgrams = pack_au(au, 1400, future_rt);
    for (const auto &dg : dgrams)
    {
        feed_depay_rtp(depay, dg);
    }
    EXPECT_EQ(0, depay_output_capture_mono(depay));
    EXPECT_EQ(1U, depay_query_u64(depay, "capture_ts_rejected"));
    depay.close();
}

TEST(RtpH264Test, DepayRejectsCaptureTooOld)
{
    vstreamer::rtp_h264_depay depay;
    ASSERT_EQ(0, depay.open());

    const uint8_t idr[] = {0x65, 0x01};
    std::vector<uint8_t> au;
    append_nal(&au, idr, sizeof(idr));
    const int64_t stale_rt = vstreamer::realtime_ns() - 120'000'000'000LL;
    const auto    dgrams = pack_au(au, 1400, stale_rt);
    for (const auto &dg : dgrams)
    {
        feed_depay_rtp(depay, dg);
    }
    EXPECT_EQ(0, depay_output_capture_mono(depay));
    EXPECT_EQ(1U, depay_query_u64(depay, "capture_ts_rejected"));
    depay.close();
}

TEST(RtpH264Test, DepayAcceptsSmallClockAheadSkew)
{
    vstreamer::rtp_h264_depay depay;
    ASSERT_EQ(0, depay.open());

    const uint8_t idr[] = {0x65, 0x01};
    std::vector<uint8_t> au;
    append_nal(&au, idr, sizeof(idr));
    const int64_t ahead_rt = vstreamer::realtime_ns() + 20'000'000LL;
    const auto    dgrams = pack_au(au, 1400, ahead_rt);
    for (const auto &dg : dgrams)
    {
        feed_depay_rtp(depay, dg);
    }
    const int64_t cap_mono = depay_output_capture_mono(depay);
    EXPECT_GT(cap_mono, 0);
    const int64_t lag_ns = vstreamer::steady_mono_ns() - cap_mono;
    EXPECT_LT(lag_ns, 0);
    EXPECT_GT(lag_ns, -35'000'000LL);
    depay.close();
}

TEST(RtpH264Test, CaptureTsUsToRtpTimestampDelta)
{
    const uint8_t idr[] = {0x65, 0x01};
    std::vector<uint8_t> au;
    append_nal(&au, idr, sizeof(idr));

    rtp_h264_config cfg;
    cfg.mtu = 1400;
    cfg.fps = 30;
    rtp_h264_packer packer(cfg);
    uint8_t buf[2048];

    const uint32_t rtp0 = static_cast<uint32_t>((33333ULL * 90ULL) / 1000ULL);
    const uint32_t rtp1 = static_cast<uint32_t>((66666ULL * 90ULL) / 1000ULL);
    ASSERT_EQ(0, packer.pack_annexb_rtp_ts(au.data(), au.size(), rtp0, 0));
    ASSERT_GT(packer.pop_datagram(buf, sizeof(buf)), 0);
    const uint32_t read0 = (static_cast<uint32_t>(buf[4]) << 24) |
                           (static_cast<uint32_t>(buf[5]) << 16) |
                           (static_cast<uint32_t>(buf[6]) << 8) |
                           static_cast<uint32_t>(buf[7]);

    packer.reset();
    ASSERT_EQ(0, packer.pack_annexb_rtp_ts(au.data(), au.size(), rtp1, 0));
    ASSERT_GT(packer.pop_datagram(buf, sizeof(buf)), 0);
    const uint32_t read1 = (static_cast<uint32_t>(buf[4]) << 24) |
                           (static_cast<uint32_t>(buf[5]) << 16) |
                           (static_cast<uint32_t>(buf[6]) << 8) |
                           static_cast<uint32_t>(buf[7]);

    EXPECT_EQ(rtp0, read0);
    EXPECT_EQ(rtp1, read1);
    EXPECT_NEAR(static_cast<int>(read1 - read0), 3000, 1);
}
