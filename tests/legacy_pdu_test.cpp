#include "apps/common/legacy_pdu.hpp"

#include <gtest/gtest.h>

#include <cerrno>

#include "core/data_packet.hpp"
#include "core/packet_types.hpp"

namespace
{

vstreamer::data_packet make_nv12_frame(int w, int h, int64_t capture_mono_ns, bool key)
{
    auto body = std::make_shared<vstreamer::frame_data>();
    body->kind = vstreamer::media_kind_e::NV12;
    body->width = w;
    body->height = h;
    body->capture_mono_ns = capture_mono_ns;
    body->key = key;
    body->buf = vstreamer::shared_sized_buffer::allocate(64);
    body->buf.resize(8);
    return vstreamer::data_packet(body);
}

}  // namespace

TEST(LegacyPduTest, FrameRoundTrip)
{
    vstreamer::apps::legacy_to_pdu to_pdu;
    std::vector<vstreamer::component_pdu> pdus;
    const int64_t cap_ns = 5'000'000'000LL;
    to_pdu.convert(make_nv12_frame(640, 480, cap_ns, true), &pdus);
    EXPECT_EQ(2u, pdus.size());
    EXPECT_TRUE(vstreamer::is_caps(pdus[0].sdu_type));
    EXPECT_EQ(vstreamer::sdu_type_e::NV12, pdus[1].sdu_type);
    EXPECT_EQ(static_cast<uint64_t>(cap_ns / 1000), pdus[1].ts_us);
    EXPECT_TRUE(vstreamer::has_flag(pdus[1], vstreamer::pdu_flag_e::KEY));

    vstreamer::apps::pdu_to_legacy to_legacy;
    vstreamer::data_packet legacy;
    EXPECT_EQ(-EAGAIN, to_legacy.convert(pdus[0], &legacy));
    EXPECT_EQ(0, to_legacy.convert(pdus[1], &legacy));
    EXPECT_EQ(vstreamer::packet_kind_e::FRAME, legacy.get_type());
    const vstreamer::frame_data &f = vstreamer::data_packet::cast<vstreamer::frame_data>(legacy);
    EXPECT_EQ(640, f.width);
    EXPECT_EQ(480, f.height);
    EXPECT_EQ(cap_ns, f.capture_mono_ns);
    EXPECT_TRUE(f.key);
}

TEST(LegacyPduTest, CapsEmittedAgainOnSizeChange)
{
    vstreamer::apps::legacy_to_pdu to_pdu;
    std::vector<vstreamer::component_pdu> pdus;
    to_pdu.convert(make_nv12_frame(640, 480, 1000, false), &pdus);
    EXPECT_EQ(2u, pdus.size());
    pdus.clear();
    to_pdu.convert(make_nv12_frame(640, 480, 2000, false), &pdus);
    EXPECT_EQ(1u, pdus.size());
    pdus.clear();
    to_pdu.convert(make_nv12_frame(320, 240, 3000, false), &pdus);
    EXPECT_EQ(2u, pdus.size());
    EXPECT_TRUE(vstreamer::is_caps(pdus[0].sdu_type));
}

TEST(LegacyPduTest, SockRoundTrip)
{
    auto body = std::make_shared<vstreamer::sock_data>();
    body->pts = 42;
    body->buf = vstreamer::shared_sized_buffer::copy_from("rtp", 3);
    const vstreamer::data_packet sock(body);

    vstreamer::apps::legacy_to_pdu to_pdu(vstreamer::sdu_type_e::RTP);
    std::vector<vstreamer::component_pdu> pdus;
    to_pdu.convert(sock, &pdus);
    EXPECT_EQ(1u, pdus.size());
    EXPECT_EQ(vstreamer::sdu_type_e::RTP, pdus[0].sdu_type);

    vstreamer::apps::pdu_to_legacy to_legacy;
    vstreamer::data_packet out;
    EXPECT_EQ(0, to_legacy.convert(pdus[0], &out));
    EXPECT_EQ(vstreamer::packet_kind_e::SOCK, out.get_type());
    EXPECT_EQ(3u, vstreamer::data_packet::cast<vstreamer::sock_data>(out).buf.size());
}
