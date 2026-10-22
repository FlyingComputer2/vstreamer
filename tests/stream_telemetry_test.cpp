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
    r.counters.udp_bytes_received = 0x3132333435363738ULL;
    r.counters.fec_bytes_received = 0x393A3B3C3D3E3F40ULL;
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
    EXPECT_TRUE(out.has_byte_totals);
    EXPECT_EQ(in.counters.udp_bytes_received, out.counters.udp_bytes_received);
    EXPECT_EQ(in.counters.fec_bytes_received, out.counters.fec_bytes_received);
}

TEST(StreamTelemetryTest, GoldenWireLayout)
{
    const vstreamer::stream_link_report in = sample_report();
    uint8_t                             wire[vstreamer::k_stream_link_report_payload_len];
    vstreamer::stream_link_report_encode_payload(in, wire, sizeof(wire));

    const uint8_t expected[vstreamer::k_stream_link_report_payload_len] = {
        0x56, 0x4C, 0x02, 0x00, 0x01, 0x02, 0x03, 0x04, 0x09, 0x0A, 0x0B,
        0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
        0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21,
        0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C,
        0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
        0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40};
    EXPECT_EQ(0, std::memcmp(wire, expected, sizeof(expected)));
}

TEST(StreamTelemetryTest, PayloadLengthsAreVersioned)
{
    EXPECT_EQ(64U, vstreamer::k_stream_link_report_payload_len);
    EXPECT_EQ(44U, vstreamer::k_stream_link_report_v1_payload_len);
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
    wire[4] = 0;
    wire[5] = 0;
    wire[6] = 0;
    wire[7] = 0;
    vstreamer::stream_link_report out {};
    EXPECT_EQ(-EPROTO, vstreamer::stream_link_report_decode(wire, sizeof(wire), &out));
}

TEST(StreamTelemetryTest, RejectNullData)
{
    vstreamer::stream_link_report rep {};
    EXPECT_EQ(-EINVAL, vstreamer::stream_link_report_decode(nullptr, 44, &rep));
}

TEST(StreamTelemetryTest, LegacyReportHasNoByteTotals)
{
    uint8_t wire[vstreamer::k_stream_link_report_payload_len];
    vstreamer::stream_link_report_encode_payload(sample_report(), wire,
                                                 sizeof(wire));
    vstreamer::stream_link_report report;
    ASSERT_EQ(0, vstreamer::stream_link_report_decode(
                     wire + 4, vstreamer::k_stream_link_report_v1_payload_len,
                     &report));
    EXPECT_FALSE(report.has_byte_totals);
    EXPECT_EQ(sample_report().counters.udp_packet_received,
              report.counters.udp_packet_received);
    EXPECT_EQ(0u, report.counters.udp_bytes_received);
}

TEST(StreamTelemetryTest, RejectUnknownVersionAndReservedBits)
{
    uint8_t wire[vstreamer::k_stream_link_report_payload_len];
    vstreamer::stream_link_report_encode_payload(sample_report(), wire,
                                                 sizeof(wire));
    vstreamer::stream_link_report report;
    wire[2] = 3;
    EXPECT_EQ(-EPROTO, vstreamer::stream_link_report_decode(wire, sizeof(wire),
                                                            &report));
    wire[2] = 2;
    wire[3] = 1;
    EXPECT_EQ(-EPROTO, vstreamer::stream_link_report_decode(wire, sizeof(wire),
                                                            &report));
}
