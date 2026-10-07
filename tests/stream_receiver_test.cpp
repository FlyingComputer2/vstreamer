#include "components/stream_receiver.hpp"

#include "core/component_pdu.hpp"
#include "core/rs_block_erasure.hpp"
#include "core/stream_header.hpp"
#include "core/stream_telemetry.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

TEST(StreamReceiverTest, OutputUnblocksOnClose)
{
    vstreamer::stream_receiver receiver;
    ASSERT_EQ(0, receiver.configure("listen", "127.0.0.1:0"));

    std::thread blocked([&receiver]() {
        vstreamer::data_packet out;
        const int              rc = receiver.output(0, out, -1);
        EXPECT_EQ(-EBADF, rc);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    receiver.close();

    blocked.join();
}

namespace
{

int ephemeral_udp_port()
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return -1;
    }
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    if (getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) < 0)
    {
        close(fd);
        return -1;
    }
    const int port = static_cast<int>(ntohs(addr.sin_port));
    close(fd);
    return port;
}

}  // namespace

TEST(StreamReceiverTest, OpenCloseStress)
{
    for (int i = 0; i < 200; ++i)
    {
        const int port = ephemeral_udp_port();
        ASSERT_GT(port, 0);
        const std::string listen = "127.0.0.1:" + std::to_string(port);

        vstreamer::stream_receiver receiver;
        ASSERT_EQ(0, receiver.configure("listen", listen));
        ASSERT_EQ(0, receiver.open());
        receiver.close();
    }
}

TEST(StreamReceiverTest, LoopbackRxTruncatedZero)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);
    const std::string listen = "127.0.0.1:" + std::to_string(port);

    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(fd, 0);

    vstreamer::stream_receiver receiver;
    ASSERT_EQ(0, receiver.configure("listen", listen));
    ASSERT_EQ(0, receiver.open());

    const char *host = "127.0.0.1";

    sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(port));
    ASSERT_EQ(1, inet_pton(AF_INET, host, &dst.sin_addr));

    uint8_t payload[512];
    std::memset(payload, 0xAB, sizeof(payload));
    const ssize_t sent =
        sendto(fd, payload, sizeof(payload), 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
    ASSERT_EQ(static_cast<ssize_t>(sizeof(payload)), sent);

    for (int attempt = 0; attempt < 50; ++attempt)
    {
        vstreamer::data_packet out;
        if (receiver.output(0, out, 20) == 0)
        {
            break;
        }
    }

    std::string trunc;
    ASSERT_EQ(0, receiver.query("rx_truncated", &trunc));
    EXPECT_EQ("0", trunc);

    close(fd);
    receiver.close();
}

namespace
{

std::vector<uint8_t> make_valid_media_datagram(uint16_t stream_seq,
                                              const std::vector<uint8_t> &app)
{
    vstreamer::rs_block_erasure enc;
    const size_t                shard_max = vstreamer::stream_max_fec_shard(1500);
    if (!enc.init(1, 1, 20, shard_max))
    {
        return {};
    }
    std::vector<std::vector<uint8_t>> air;
    enc.push_app(app.data(), app.size(), &air);
    if (air.empty())
    {
        return {};
    }
    std::vector<uint8_t> wire(vstreamer::k_stream_header_len + air[0].size());
    vstreamer::stream_header hdr {};
    hdr.sequence_number = stream_seq;
    hdr.is_fec = true;
    hdr.is_stream_data = true;
    hdr.ext_len = 0;
    vstreamer::stream_header_write(wire.data(), hdr);
    std::memcpy(wire.data() + vstreamer::k_stream_header_len, air[0].data(), air[0].size());
    return wire;
}

bool recv_link_report_wire(int fd, vstreamer::stream_link_report *out, int timeout_ms)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    uint8_t buf[128];
    const size_t expect =
        vstreamer::k_stream_header_len + vstreamer::k_stream_link_report_payload_len;
    while (std::chrono::steady_clock::now() < deadline)
    {
        pollfd pfd {fd, POLLIN, 0};
        if (poll(&pfd, 1, 50) <= 0)
        {
            continue;
        }
        const ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n != static_cast<ssize_t>(expect))
        {
            continue;
        }
        vstreamer::stream_header hdr {};
        const uint8_t *payload = nullptr;
        size_t         payload_len = 0;
        if (vstreamer::stream_header_parse(buf, static_cast<size_t>(n), &hdr, &payload,
                                           &payload_len) < 0)
        {
            continue;
        }
        if (hdr.is_fec || hdr.is_stream_data)
        {
            continue;
        }
        if (vstreamer::stream_link_report_decode(payload, payload_len, out) < 0)
        {
            continue;
        }
        out->report_seq = hdr.sequence_number;
        return true;
    }
    return false;
}

}  // namespace

TEST(StreamReceiverTest, TelemetryWaitsForValidMedia)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(s, 0);

    vstreamer::stream_receiver receiver;
    ASSERT_EQ(0, receiver.configure("listen", "127.0.0.1:" + std::to_string(port)));
    ASSERT_EQ(0, receiver.configure("telemetry_ms", "20"));
    ASSERT_EQ(0, receiver.open());

    vstreamer::stream_link_report rep {};
    EXPECT_FALSE(recv_link_report_wire(s, &rep, 100));

    receiver.close();
    close(s);
}

TEST(StreamReceiverTest, TelemetryReportsAfterMedia)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(s, 0);

    vstreamer::stream_receiver receiver;
    ASSERT_EQ(0, receiver.configure("listen", "127.0.0.1:" + std::to_string(port)));
    ASSERT_EQ(0, receiver.configure("telemetry_ms", "20"));
    ASSERT_EQ(0, receiver.open());

    sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(port));
    ASSERT_EQ(1, inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr));

    const std::vector<uint8_t> app(32, 0xCD);
    const auto                 wire = make_valid_media_datagram(1, app);
    ASSERT_FALSE(wire.empty());
    ASSERT_EQ(static_cast<ssize_t>(wire.size()),
              sendto(s, wire.data(), wire.size(), 0, reinterpret_cast<sockaddr *>(&dst),
                     sizeof(dst)));

    std::string session_s;
    ASSERT_EQ(0, receiver.query("session_id", &session_s));
    const uint32_t session = static_cast<uint32_t>(std::strtoul(session_s.c_str(), nullptr, 10));
    ASSERT_NE(0U, session);

    int           got = 0;
    uint32_t      last_seq = UINT32_MAX;
    const auto    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (got < 5 && std::chrono::steady_clock::now() < deadline)
    {
        vstreamer::stream_link_report rep {};
        if (!recv_link_report_wire(s, &rep, 50))
        {
            continue;
        }
        if (got == 0)
        {
            last_seq = rep.report_seq;
        }
        else
        {
            EXPECT_EQ(last_seq + 1, rep.report_seq);
            last_seq = rep.report_seq;
        }
        EXPECT_EQ(session, rep.session_id);
        EXPECT_EQ(1U, rep.counters.udp_packet_received);
        ++got;
    }
    EXPECT_GE(got, 5);

    receiver.close();
    close(s);
}

TEST(StreamReceiverTest, TelemetryPeerFollowsLatestSender)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    const int s1 = socket(AF_INET, SOCK_DGRAM, 0);
    const int s2 = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(s1, 0);
    ASSERT_GE(s2, 0);

    vstreamer::stream_receiver receiver;
    ASSERT_EQ(0, receiver.configure("listen", "127.0.0.1:" + std::to_string(port)));
    ASSERT_EQ(0, receiver.configure("telemetry_ms", "20"));
    ASSERT_EQ(0, receiver.open());

    sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(port));
    ASSERT_EQ(1, inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr));

    const std::vector<uint8_t> app(32, 0x11);
    const auto                 wire = make_valid_media_datagram(1, app);
    ASSERT_FALSE(wire.empty());
    sendto(s1, wire.data(), wire.size(), 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));

    vstreamer::stream_link_report rep {};
    ASSERT_TRUE(recv_link_report_wire(s1, &rep, 200));

    sendto(s2, wire.data(), wire.size(), 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
    ASSERT_TRUE(recv_link_report_wire(s2, &rep, 200));
    EXPECT_FALSE(recv_link_report_wire(s1, &rep, 80));

    std::string changes;
    ASSERT_EQ(0, receiver.query("telemetry_peer_changes", &changes));
    EXPECT_EQ("1", changes);

    receiver.close();
    close(s1);
    close(s2);
}

TEST(StreamReceiverTest, TelemetryInvalidMediaDoesNotMovePeer)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    const int s3 = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(s, 0);
    ASSERT_GE(s3, 0);

    vstreamer::stream_receiver receiver;
    ASSERT_EQ(0, receiver.configure("listen", "127.0.0.1:" + std::to_string(port)));
    ASSERT_EQ(0, receiver.configure("telemetry_ms", "20"));
    ASSERT_EQ(0, receiver.open());

    sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(port));
    ASSERT_EQ(1, inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr));

    const std::vector<uint8_t> app(32, 0x22);
    const auto                 wire = make_valid_media_datagram(1, app);
    sendto(s, wire.data(), wire.size(), 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
    vstreamer::stream_link_report rep {};
    ASSERT_TRUE(recv_link_report_wire(s, &rep, 200));

    const char garbage[] = "bad";
    sendto(s3, garbage, sizeof(garbage), 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
    ASSERT_TRUE(recv_link_report_wire(s, &rep, 200));
    EXPECT_FALSE(recv_link_report_wire(s3, &rep, 80));

    /* Long enough for a stream header + FEC header, but the FEC header does not parse (k = 0). */
    for (const size_t junk_len : {size_t {8}, size_t {64}})
    {
        const std::vector<uint8_t> junk(junk_len, 0);
        sendto(s3, junk.data(), junk.size(), 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
        ASSERT_TRUE(recv_link_report_wire(s, &rep, 200)) << "junk_len=" << junk_len;
        EXPECT_FALSE(recv_link_report_wire(s3, &rep, 80)) << "junk_len=" << junk_len;
    }
    std::string changes;
    ASSERT_EQ(0, receiver.query("telemetry_peer_changes", &changes));
    EXPECT_EQ("0", changes);

    receiver.close();
    close(s);
    close(s3);
}

TEST(StreamReceiverTest, TelemetrySessionResetsOnReopen)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);
    const std::string listen = "127.0.0.1:" + std::to_string(port);

    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(s, 0);

    vstreamer::stream_receiver receiver;
    ASSERT_EQ(0, receiver.configure("listen", listen));
    ASSERT_EQ(0, receiver.configure("telemetry_ms", "20"));
    ASSERT_EQ(0, receiver.open());

    sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(port));
    ASSERT_EQ(1, inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr));

    const std::vector<uint8_t> app(32, 0x33);
    const auto                 wire = make_valid_media_datagram(1, app);
    sendto(s, wire.data(), wire.size(), 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));

    std::string session_a;
    ASSERT_EQ(0, receiver.query("session_id", &session_a));
    vstreamer::stream_link_report rep {};
    ASSERT_TRUE(recv_link_report_wire(s, &rep, 200));
    EXPECT_EQ(0U, rep.report_seq);

    receiver.close();
    ASSERT_EQ(0, receiver.configure("listen", listen));
    ASSERT_EQ(0, receiver.open());
    std::string session_b;
    ASSERT_EQ(0, receiver.query("session_id", &session_b));
    EXPECT_NE(session_a, session_b);

    EXPECT_FALSE(recv_link_report_wire(s, &rep, 100));
    sendto(s, wire.data(), wire.size(), 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
    ASSERT_TRUE(recv_link_report_wire(s, &rep, 200));
    EXPECT_EQ(0U, rep.report_seq);

    receiver.close();
    close(s);
}

TEST(StreamReceiverTest, TelemetryConfigureValidation)
{
    vstreamer::stream_receiver receiver;
    ASSERT_EQ(0, receiver.configure("telemetry", "off"));
    ASSERT_LT(receiver.configure("telemetry_ms", "19"), 0);
    ASSERT_LT(receiver.configure("telemetry_ms", "5001"), 0);
    ASSERT_EQ(0, receiver.configure("telemetry_ms", "20"));
}

TEST(StreamReceiverTest, TelemetryOffSuppressesReports)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(s, 0);

    vstreamer::stream_receiver receiver;
    ASSERT_EQ(0, receiver.configure("listen", "127.0.0.1:" + std::to_string(port)));
    ASSERT_EQ(0, receiver.configure("telemetry", "off"));
    ASSERT_EQ(0, receiver.configure("telemetry_ms", "20"));
    ASSERT_EQ(0, receiver.open());

    sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(port));
    ASSERT_EQ(1, inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr));

    const std::vector<uint8_t> app(32, 0x44);
    const auto                 wire = make_valid_media_datagram(1, app);
    sendto(s, wire.data(), wire.size(), 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));

    vstreamer::stream_link_report rep {};
    EXPECT_FALSE(recv_link_report_wire(s, &rep, 150));

    receiver.close();
    close(s);
}

/* Telemetry counters are per open(), like the link counters. */
TEST(StreamReceiverTest, TelemetryCountersResetOnReopen)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);
    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(s, 0);
    vstreamer::stream_receiver receiver;
    ASSERT_EQ(0, receiver.configure("listen", "127.0.0.1:" + std::to_string(port)));
    ASSERT_EQ(0, receiver.configure("telemetry_ms", "20"));
    ASSERT_EQ(0, receiver.open());
    sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(port));
    ASSERT_EQ(1, inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr));
    const auto wire = make_valid_media_datagram(1, std::vector<uint8_t>(32, 0x22));
    sendto(s, wire.data(), wire.size(), 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
    vstreamer::stream_link_report rep {};
    ASSERT_TRUE(recv_link_report_wire(s, &rep, 200));
    std::string sent;
    ASSERT_EQ(0, receiver.query("telemetry_sent", &sent));
    EXPECT_NE("0", sent);

    receiver.close();
    ASSERT_EQ(0, receiver.open());
    ASSERT_EQ(0, receiver.query("telemetry_sent", &sent));
    EXPECT_EQ("0", sent);
    receiver.close();
    close(s);
}

TEST(StreamReceiverTest, BadHeaderCountedAndDoesNotMoveTelemetryPeer)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    const int s_bad = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(s, 0);
    ASSERT_GE(s_bad, 0);

    vstreamer::stream_receiver receiver;
    ASSERT_EQ(0, receiver.configure("listen", "127.0.0.1:" + std::to_string(port)));
    ASSERT_EQ(0, receiver.configure("telemetry_ms", "20"));
    ASSERT_EQ(0, receiver.open());

    sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(port));
    ASSERT_EQ(1, inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr));

    const auto good = make_valid_media_datagram(1, std::vector<uint8_t>(32, 0x55));
    sendto(s, good.data(), good.size(), 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
    vstreamer::stream_link_report rep {};
    ASSERT_TRUE(recv_link_report_wire(s, &rep, 200));

    const uint8_t v1ish[] = {0, 1, 0, 0, 0, 0, 0, 0};
    sendto(s_bad, v1ish, sizeof(v1ish), 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));

    uint8_t tele[vstreamer::k_stream_header_len + vstreamer::k_stream_link_report_payload_len] {};
    vstreamer::stream_header th {};
    th.sequence_number = 1;
    th.is_fec = false;
    th.is_stream_data = false;
    vstreamer::stream_header_write(tele, th);
    sendto(s_bad, tele, sizeof(tele), 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    std::string bad;
    ASSERT_EQ(0, receiver.query("rx_bad_header", &bad));
    EXPECT_GE(std::strtoull(bad.c_str(), nullptr, 10), 2ULL);
    ASSERT_TRUE(recv_link_report_wire(s, &rep, 200));
    EXPECT_FALSE(recv_link_report_wire(s_bad, &rep, 80));

    receiver.close();
    close(s);
    close(s_bad);
}

TEST(StreamReceiverTest, PortCapsAdvertisesStreamDgram)
{
    vstreamer::stream_receiver receiver;
    std::string                val;
    ASSERT_EQ(0, receiver.query("outport-0.caps-0.sdu_type", &val));
    EXPECT_EQ("STREAM_DGRAM", val);
}

TEST(StreamReceiverTest, PduOutputNonBlocking)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    vstreamer::stream_receiver receiver;
    ASSERT_EQ(0, receiver.configure("listen", "127.0.0.1:" + std::to_string(port)));
    ASSERT_EQ(0, receiver.open());

    vstreamer::component_pdu pdu;
    EXPECT_EQ(-EAGAIN, receiver.output(pdu));

    receiver.close();
}

TEST(StreamReceiverTest, PduDiscontAfterUdpSequenceGap)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(s, 0);

    vstreamer::stream_receiver receiver;
    ASSERT_EQ(0, receiver.configure("listen", "127.0.0.1:" + std::to_string(port)));
    ASSERT_EQ(0, receiver.open());

    sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(port));
    ASSERT_EQ(1, inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr));

    const auto d1 = make_valid_media_datagram(1, std::vector<uint8_t>(24, 0x11));
    const auto d4 = make_valid_media_datagram(4, std::vector<uint8_t>(24, 0x22));
    sendto(s, d1.data(), d1.size(), 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
    sendto(s, d4.data(), d4.size(), 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));

    vstreamer::component_pdu first;
    vstreamer::component_pdu second;
    const auto               deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool                     have_first = false;
    bool                     have_second = false;
    while (std::chrono::steady_clock::now() < deadline && (!have_first || !have_second))
    {
        if (!have_first)
        {
            vstreamer::component_pdu pdu;
            if (0 == receiver.output(pdu))
            {
                first = std::move(pdu);
                have_first = true;
            }
        }
        if (have_first && !have_second)
        {
            vstreamer::component_pdu pdu;
            if (0 == receiver.output(pdu))
            {
                second = std::move(pdu);
                have_second = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(have_first);
    ASSERT_TRUE(have_second);
    EXPECT_EQ(vstreamer::sdu_type_e::STREAM_DGRAM, first.sdu_type);
    EXPECT_EQ(vstreamer::sdu_type_e::STREAM_DGRAM, second.sdu_type);
    EXPECT_FALSE(vstreamer::has_flag(first, vstreamer::pdu_flag_e::DISCONT));
    EXPECT_TRUE(vstreamer::has_flag(second, vstreamer::pdu_flag_e::DISCONT));
    EXPECT_GT(second.ts_us, 0ULL);

    receiver.close();
    close(s);
}
