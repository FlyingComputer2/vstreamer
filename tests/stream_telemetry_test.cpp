#include "core/stream_telemetry.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace
{

vstreamer::stream_link_report sample_report()
{
    vstreamer::stream_link_report r;
    r.session_id = 0x01020304U;
    r.report_seq = 0x0506U;
    r.timestamp_us = 0x090A0B0C0D0E0F10ULL;
    r.counters.udp_packet_received = 0x1112131415161718ULL;
    r.counters.udp_gap_count = 0x191A1B1C1D1E1F20ULL;
    r.counters.fec_packet_received = 0x2122232425262728ULL;
    r.counters.fec_gap_count = 0x292A2B2C2D2E2F30ULL;
    return r;
}

}  // namespace

TEST(StreamTelemetryTest, RoundTripAllFields)
{
    const vstreamer::stream_link_report in = sample_report();
    uint8_t                             wire[vstreamer::k_stream_link_report_payload_len];
    vstreamer::stream_link_report_encode_payload(in, wire, sizeof(wire));

    vstreamer::stream_link_report out {};
    EXPECT_EQ(0, vstreamer::stream_link_report_decode(wire, sizeof(wire), &out));
    EXPECT_EQ(in.session_id, out.session_id);
    EXPECT_EQ(in.timestamp_us, out.timestamp_us);
    EXPECT_EQ(in.counters.udp_packet_received, out.counters.udp_packet_received);
    EXPECT_EQ(in.counters.udp_gap_count, out.counters.udp_gap_count);
    EXPECT_EQ(in.counters.fec_packet_received, out.counters.fec_packet_received);
    EXPECT_EQ(in.counters.fec_gap_count, out.counters.fec_gap_count);
}

TEST(StreamTelemetryTest, GoldenWireLayout)
{
    const vstreamer::stream_link_report in = sample_report();
    uint8_t                             wire[vstreamer::k_stream_link_report_payload_len];
    vstreamer::stream_link_report_encode_payload(in, wire, sizeof(wire));

    const uint8_t expected[vstreamer::k_stream_link_report_payload_len] = {
        0x01, 0x02, 0x03, 0x04, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12,
        0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20,
        0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E,
        0x2F, 0x30};
    EXPECT_EQ(0, std::memcmp(wire, expected, sizeof(expected)));
}

TEST(StreamTelemetryTest, PayloadLenIs44)
{
    EXPECT_EQ(44U, vstreamer::k_stream_link_report_payload_len);
}

TEST(StreamTelemetryTest, RejectBadLength)
{
    uint8_t wire[vstreamer::k_stream_link_report_payload_len];
    vstreamer::stream_link_report_encode_payload(sample_report(), wire, sizeof(wire));
    vstreamer::stream_link_report out {};
    EXPECT_EQ(-EMSGSIZE, vstreamer::stream_link_report_decode(wire, 43, &out));
    EXPECT_EQ(-EMSGSIZE, vstreamer::stream_link_report_decode(wire, 45, &out));
}

TEST(StreamTelemetryTest, RejectZeroSession)
{
    uint8_t wire[vstreamer::k_stream_link_report_payload_len];
    vstreamer::stream_link_report_encode_payload(sample_report(), wire, sizeof(wire));
    wire[0] = 0;
    wire[1] = 0;
    wire[2] = 0;
    wire[3] = 0;
    vstreamer::stream_link_report out {};
    EXPECT_EQ(-EPROTO, vstreamer::stream_link_report_decode(wire, sizeof(wire), &out));
}

TEST(StreamTelemetryTest, RejectNullData)
{
    vstreamer::stream_link_report rep {};
    EXPECT_EQ(-EINVAL, vstreamer::stream_link_report_decode(nullptr, 44, &rep));
}
