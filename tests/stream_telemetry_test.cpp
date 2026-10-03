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
    r.report_seq = 0x05060708U;
    r.interval_ms = 100;
    r.counters.udp_packet_received = 0x090A0B0C0D0E0F10ULL;
    r.counters.udp_gap_count = 0x1112131415161718ULL;
    r.counters.fec_packet_received = 0x191A1B1C1D1E1F20ULL;
    r.counters.fec_gap_count = 0x2122232425262728ULL;
    return r;
}

}  // namespace

TEST(StreamTelemetryTest, RoundTripAllFields)
{
    const vstreamer::stream_link_report in = sample_report();
    uint8_t                             wire[vstreamer::k_stream_link_report_len];
    vstreamer::stream_link_report_encode(in, wire);

    vstreamer::stream_link_report out {};
    EXPECT_EQ(0, vstreamer::stream_link_report_decode(wire, sizeof(wire), &out));
    EXPECT_EQ(in.session_id, out.session_id);
    EXPECT_EQ(in.report_seq, out.report_seq);
    EXPECT_EQ(in.interval_ms, out.interval_ms);
    EXPECT_EQ(in.counters.udp_packet_received, out.counters.udp_packet_received);
    EXPECT_EQ(in.counters.udp_gap_count, out.counters.udp_gap_count);
    EXPECT_EQ(in.counters.fec_packet_received, out.counters.fec_packet_received);
    EXPECT_EQ(in.counters.fec_gap_count, out.counters.fec_gap_count);
}

TEST(StreamTelemetryTest, GoldenWireLayout)
{
    const vstreamer::stream_link_report in = sample_report();
    uint8_t                             wire[vstreamer::k_stream_link_report_len];
    vstreamer::stream_link_report_encode(in, wire);

    const uint8_t expected[vstreamer::k_stream_link_report_len] = {
        0x56, 0x54, 0x01, 0x01, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x00, 0x64,
        0x00, 0x00, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14,
        0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22,
        0x23, 0x24, 0x25, 0x26, 0x27, 0x28};
    EXPECT_EQ(0, std::memcmp(wire, expected, sizeof(expected)));
}

TEST(StreamTelemetryTest, RejectBadLength)
{
    uint8_t wire[vstreamer::k_stream_link_report_len];
    vstreamer::stream_link_report_encode(sample_report(), wire);
    vstreamer::stream_link_report out {};
    EXPECT_EQ(-EMSGSIZE, vstreamer::stream_link_report_decode(wire, 47, &out));
    EXPECT_EQ(-EMSGSIZE, vstreamer::stream_link_report_decode(wire, 49, &out));
}

TEST(StreamTelemetryTest, RejectBadHeader)
{
    uint8_t wire[vstreamer::k_stream_link_report_len];
    vstreamer::stream_link_report_encode(sample_report(), wire);
    vstreamer::stream_link_report out {};

    wire[0] = 0x00;
    EXPECT_EQ(-EPROTO, vstreamer::stream_link_report_decode(wire, sizeof(wire), &out));

    vstreamer::stream_link_report_encode(sample_report(), wire);
    wire[2] = 2;
    EXPECT_EQ(-EPROTO, vstreamer::stream_link_report_decode(wire, sizeof(wire), &out));

    vstreamer::stream_link_report_encode(sample_report(), wire);
    wire[3] = 0;
    EXPECT_EQ(-EPROTO, vstreamer::stream_link_report_decode(wire, sizeof(wire), &out));

    vstreamer::stream_link_report_encode(sample_report(), wire);
    wire[4] = 0;
    wire[5] = 0;
    wire[6] = 0;
    wire[7] = 0;
    EXPECT_EQ(-EPROTO, vstreamer::stream_link_report_decode(wire, sizeof(wire), &out));
}

TEST(StreamTelemetryTest, NonZeroReservedAccepted)
{
    uint8_t wire[vstreamer::k_stream_link_report_len];
    vstreamer::stream_link_report_encode(sample_report(), wire);
    wire[14] = 0xAB;
    wire[15] = 0xCD;
    vstreamer::stream_link_report out {};
    EXPECT_EQ(0, vstreamer::stream_link_report_decode(wire, sizeof(wire), &out));
    EXPECT_EQ(sample_report().session_id, out.session_id);
}
