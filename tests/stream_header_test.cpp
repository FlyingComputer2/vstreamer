#include "core/stream_header.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <vector>

namespace
{

vstreamer::stream_header sample_hdr()
{
    vstreamer::stream_header h {};
    h.sequence_number = 0xABCD;
    h.is_fec = true;
    h.is_stream_data = true;
    h.ext_len = 0;
    return h;
}

}  // namespace

TEST(StreamHeaderTest, RoundTripFixedHeader)
{
    const vstreamer::stream_header in = sample_hdr();
    uint8_t                          wire[vstreamer::k_stream_header_len];
    vstreamer::stream_header_write(wire, in);

    EXPECT_EQ(0xAB, wire[0]);
    EXPECT_EQ(0xCD, wire[1]);
    EXPECT_EQ(0x1E, wire[2]); /* v3, fec, stream */
    EXPECT_EQ(0x00, wire[3]);

    vstreamer::stream_header          out {};
    const uint8_t                    *payload = nullptr;
    size_t                            payload_len = 0;
    uint8_t                           body[] = {0x01, 0x02, 0x03};
    std::vector<uint8_t>              datagram(wire, wire + sizeof(wire));
    datagram.insert(datagram.end(), body, body + sizeof(body));

    EXPECT_EQ(0, vstreamer::stream_header_parse(datagram.data(), datagram.size(), &out,
                                                &payload, &payload_len));
    EXPECT_EQ(in.sequence_number, out.sequence_number);
    EXPECT_EQ(in.is_fec, out.is_fec);
    EXPECT_EQ(in.is_stream_data, out.is_stream_data);
    EXPECT_EQ(0U, out.ext_len);
    EXPECT_EQ(sizeof(body), payload_len);
    EXPECT_EQ(0, std::memcmp(body, payload, sizeof(body)));
}

TEST(StreamHeaderTest, StampSequencePatchesBytesZeroOneOnly)
{
    vstreamer::stream_header h = sample_hdr();
    h.sequence_number = 0;
    uint8_t wire[vstreamer::k_stream_header_len + 1];
    vstreamer::stream_header_write(wire, h);
    wire[4] = 0x55;

    vstreamer::stream_header_stamp_sequence(wire, 0x1234);
    EXPECT_EQ(0x12, wire[0]);
    EXPECT_EQ(0x34, wire[1]);
    EXPECT_EQ(0x1E, wire[2]);
    EXPECT_EQ(0x00, wire[3]);
    EXPECT_EQ(0x55, wire[4]);
}

TEST(StreamHeaderTest, RejectTooShort)
{
    uint8_t wire[3] = {0x00, 0x01, 0x10};
    vstreamer::stream_header out {};
    const uint8_t           *payload = nullptr;
    size_t                   payload_len = 0;
    EXPECT_EQ(-EMSGSIZE,
              vstreamer::stream_header_parse(wire, sizeof(wire), &out, &payload, &payload_len));
}

TEST(StreamHeaderTest, RejectWrongVersion)
{
    uint8_t wire[vstreamer::k_stream_header_len] = {0x00, 0x01, 0x08, 0x00}; /* version 1 */
    vstreamer::stream_header out {};
    const uint8_t           *payload = nullptr;
    size_t                   payload_len = 0;
    EXPECT_EQ(-EPROTO,
              vstreamer::stream_header_parse(wire, sizeof(wire), &out, &payload, &payload_len));
}

TEST(StreamHeaderTest, RejectFlagSpareBit)
{
    uint8_t wire[vstreamer::k_stream_header_len] = {0x00, 0x01, 0x17, 0x00}; /* spare set */
    vstreamer::stream_header out {};
    const uint8_t           *payload = nullptr;
    size_t                   payload_len = 0;
    EXPECT_EQ(-EPROTO,
              vstreamer::stream_header_parse(wire, sizeof(wire), &out, &payload, &payload_len));
}

TEST(StreamHeaderTest, RejectFecTelemetryKind)
{
    vstreamer::stream_header h {};
    h.sequence_number = 1;
    h.is_fec = true;
    h.is_stream_data = false;
    uint8_t wire[vstreamer::k_stream_header_len];
    vstreamer::stream_header_write(wire, h);
    wire[2] = static_cast<uint8_t>((vstreamer::k_stream_wire_version
                                    << vstreamer::k_stream_flag_version_shift) |
                                   (1U << vstreamer::k_stream_flag_is_fec_shift));

    vstreamer::stream_header out {};
    const uint8_t           *payload = nullptr;
    size_t                   payload_len = 0;
    EXPECT_EQ(-EPROTO,
              vstreamer::stream_header_parse(wire, sizeof(wire), &out, &payload, &payload_len));
}

TEST(StreamHeaderTest, SkipUnknownExtTlv)
{
    std::vector<uint8_t> datagram(vstreamer::k_stream_header_len + 4 + 2, 0);
    datagram[2] = static_cast<uint8_t>(vstreamer::k_stream_wire_version
                                         << vstreamer::k_stream_flag_version_shift);
    datagram[3] = 4;
    datagram[4] = 0x7F; /* type */
    datagram[5] = 2;    /* len */
    datagram[6] = 0xAA;
    datagram[7] = 0xBB;
    datagram[8] = 0xCC;
    datagram[9] = 0xDD;

    vstreamer::stream_header out {};
    const uint8_t           *payload = nullptr;
    size_t                   payload_len = 0;
    EXPECT_EQ(0, vstreamer::stream_header_parse(datagram.data(), datagram.size(), &out, &payload,
                                                &payload_len));
    EXPECT_EQ(2U, payload_len);
    EXPECT_EQ(0xCC, payload[0]);
    EXPECT_EQ(0xDD, payload[1]);
}

TEST(StreamHeaderTest, RejectExtPastEndOfDatagram)
{
    uint8_t wire[vstreamer::k_stream_header_len] = {0x00, 0x01, 0x18, 0x04};
    vstreamer::stream_header out {};
    const uint8_t           *payload = nullptr;
    size_t                   payload_len = 0;
    EXPECT_EQ(-EMSGSIZE,
              vstreamer::stream_header_parse(wire, sizeof(wire), &out, &payload, &payload_len));
}

TEST(StreamHeaderTest, RejectTruncatedTlvInExt)
{
    std::vector<uint8_t> datagram(vstreamer::k_stream_header_len + 3, 0);
    datagram[2] = static_cast<uint8_t>(vstreamer::k_stream_wire_version
                                         << vstreamer::k_stream_flag_version_shift);
    datagram[3] = 3;
    datagram[4] = 0x01;
    datagram[5] = 4; /* claims 4 payload bytes but ext is only 3 */

    vstreamer::stream_header out {};
    const uint8_t           *payload = nullptr;
    size_t                   payload_len = 0;
    EXPECT_EQ(-EMSGSIZE,
              vstreamer::stream_header_parse(datagram.data(), datagram.size(), &out, &payload,
                                             &payload_len));
}

TEST(StreamHeaderTest, RejectV1StyleFecBytes)
{
    /* v1: 2-byte sequence + legacy id byte — not a v2 version field. */
    uint8_t wire[8] = {0x12, 0x34, 0x07, 0x18, 0x88, 0x04, 0x00, 0x00};
    vstreamer::stream_header out {};
    const uint8_t           *payload = nullptr;
    size_t                   payload_len = 0;
    EXPECT_EQ(-EPROTO,
              vstreamer::stream_header_parse(wire, sizeof(wire), &out, &payload, &payload_len));
}

TEST(StreamHeaderTest, MaxShardAndRawHelpers)
{
    EXPECT_EQ(1396U, vstreamer::stream_max_fec_shard(1400));
    EXPECT_EQ(1396U, vstreamer::stream_max_raw_sdu(1400));
    EXPECT_EQ(0U, vstreamer::stream_max_fec_shard(3));
}
