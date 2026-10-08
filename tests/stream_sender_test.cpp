#include "components/stream_receiver.hpp"
#include "components/stream_sender.hpp"

#include "core/component.hpp"
#include "core/component_pdu.hpp"
#include "core/shared_sized_buffer.hpp"
#include "core/rs_block_erasure.hpp"
#include "core/stream_header.hpp"
#include "core/stream_telemetry.hpp"

#include <gtest/gtest.h>

#include "test_pdu_helpers.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace
{

int cfg(vstreamer::component &c, std::string_view key, std::string_view value)
{
    return c.configure(key, value);
}

}  // namespace

TEST(StreamSenderTest, OpenWithHostnameDoesNotHang)
{
    const int port = 5091;
    vstreamer::stream_sender sender;
    const std::string        stream = "localhost:" + std::to_string(port);
    ASSERT_EQ(0, cfg(sender, "stream", stream));
    ASSERT_EQ(0, cfg(sender, "fec", "none"));

    auto fut = std::async(std::launch::async, [&sender]() { return sender.open(); });
    EXPECT_EQ(std::future_status::ready, fut.wait_for(std::chrono::seconds(2)));
    if (fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        EXPECT_EQ(0, fut.get());
        sender.close();
    }
}

TEST(StreamSenderTest, BogusHostnameReturnsQuickly)
{
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, cfg(sender, "stream", "bogus.invalid:5000"));
    auto fut = std::async(std::launch::async, [&sender]() { return sender.open(); });
    EXPECT_EQ(std::future_status::ready, fut.wait_for(std::chrono::seconds(2)));
    ASSERT_EQ(std::future_status::ready, fut.wait_for(std::chrono::seconds(0)));
    EXPECT_LT(fut.get(), 0);
}

TEST(StreamSenderTest, ConfigureLongValueRejected)
{
    vstreamer::stream_sender sender;
    const std::string        long_mtu(200, '9');
    EXPECT_LT(cfg(sender, "mtu", long_mtu), 0);
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


size_t query_size_t(const vstreamer::stream_sender &sender, const char *key)
{
    std::string val;
    EXPECT_EQ(0, sender.query(key, &val));
    return static_cast<size_t>(std::strtoull(val.c_str(), nullptr, 10));
}

}  // namespace

TEST(StreamSenderTest, QueueByteLimitWithPacing)
{
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, cfg(sender, "max_kbps", "1000"));
    ASSERT_EQ(0, cfg(sender, "queue_ms", "100"));
    const size_t limit = query_size_t(sender, "queue_byte_limit");
    EXPECT_GE(limit, 32768U);
}

TEST(StreamSenderTest, QueueByteLimitHoldsKeyframeBurstWithFec)
{
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, cfg(sender, "fec_n", "15"));
    ASSERT_EQ(0, cfg(sender, "fec_k", "8"));
    ASSERT_EQ(0, cfg(sender, "queue_ms", "100"));
    /* Four full k=8 n=15 blocks of max-size shards. */
    EXPECT_GE(query_size_t(sender, "queue_byte_limit"), 4U * 15U * 1500U);
}

TEST(StreamSenderTest, QueueByteLimitScalesWithFecN)
{
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, cfg(sender, "fec_k", "8"));
    ASSERT_EQ(0, cfg(sender, "fec_n", "15"));
    ASSERT_EQ(0, cfg(sender, "queue_ms", "100"));
    EXPECT_GE(query_size_t(sender, "queue_byte_limit"), 256U * 1024U);

    ASSERT_EQ(0, cfg(sender, "fec_n", "31"));
    const size_t min_n31 = (136U * 1024U * 31U) / 8U;
    EXPECT_GE(query_size_t(sender, "queue_byte_limit"), min_n31);
}

/* Unpaced: a keyframe's data and parity are enqueued at once. None of it may be evicted, or the
 * receiver gets the missing data shards back from parity only after later packets. */
TEST(StreamSenderTest, KeyframeBurstWithFecNotEvicted)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    vstreamer::stream_sender sender;
    const std::string        host_port = "127.0.0.1:" + std::to_string(port);
    ASSERT_EQ(0, cfg(sender, "stream", host_port));
    ASSERT_EQ(0, cfg(sender, "fec_n", "15"));
    ASSERT_EQ(0, cfg(sender, "fec_k", "8"));
    ASSERT_EQ(0, sender.open());
    ASSERT_EQ(0, sender.set_enabled(true, 0));

    constexpr int    k_packets = 32;
    constexpr size_t k_payload = 1400;
    for (int i = 0; i < k_packets; ++i)
    {
        auto pkt = vstreamer::test_pdu::make_stream_dgram(static_cast<uint32_t>(i), k_payload);
        ASSERT_EQ(0, sender.input(std::move(pkt)));
    }

    std::string dropped_s;
    ASSERT_EQ(0, sender.query("dropped", &dropped_s));
    EXPECT_EQ(0ULL, std::strtoull(dropped_s.c_str(), nullptr, 10));

    sender.close();
}

TEST(StreamSenderTest, QueueByteCapReturnsEagainUnderPacing)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    vstreamer::stream_sender sender;
    const std::string        host_port = "127.0.0.1:" + std::to_string(port);
    ASSERT_EQ(0, cfg(sender, "stream", host_port));
    ASSERT_EQ(0, cfg(sender, "fec", "none"));
    ASSERT_EQ(0, cfg(sender, "max_kbps", "100"));
    ASSERT_EQ(0, cfg(sender, "queue_ms", "100"));

    ASSERT_EQ(0, sender.open());
    ASSERT_EQ(0, sender.set_enabled(true, 0));

    const size_t limit = query_size_t(sender, "queue_byte_limit");
    constexpr size_t k_payload = 128;

    bool saw_eagain = false;
    for (int i = 0; i < 6000; ++i)
    {
        auto pkt = vstreamer::test_pdu::make_stream_dgram(static_cast<uint32_t>(i), k_payload);
        const int rc = sender.input(std::move(pkt));
        if (-EAGAIN == rc)
        {
            saw_eagain = true;
            break;
        }
        ASSERT_EQ(0, rc);
        const size_t qb = query_size_t(sender, "queue_bytes");
        EXPECT_LE(qb, limit);
    }
    EXPECT_TRUE(saw_eagain);

    std::string dropped_s;
    ASSERT_EQ(0, sender.query("dropped", &dropped_s));
    EXPECT_EQ(0ULL, std::strtoull(dropped_s.c_str(), nullptr, 10));

    sender.close();
}

TEST(StreamSenderTest, MaxKbpsReconfigureWhileSending)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    vstreamer::stream_sender   sender;
    vstreamer::stream_receiver receiver;
    const std::string          host_port = "127.0.0.1:" + std::to_string(port);
    ASSERT_EQ(0, cfg(sender, "stream", host_port));
    ASSERT_EQ(0, cfg(receiver, "listen", host_port));
    ASSERT_EQ(0, cfg(sender, "fec", "none"));
    ASSERT_EQ(0, cfg(sender, "max_kbps", "500"));

    ASSERT_EQ(0, receiver.open());
    ASSERT_EQ(0, sender.open());
    ASSERT_EQ(0, sender.set_enabled(true, 0));

    std::atomic<bool> stop {false};
    std::thread       hammer([&]() {
        for (int kbps = 100; kbps <= 2000 && !stop.load(); kbps += 50)
        {
            (void)cfg(sender, "max_kbps", std::to_string(kbps));
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    for (int i = 0; i < 500; ++i)
    {
        auto pkt = vstreamer::test_pdu::make_stream_dgram(static_cast<uint32_t>(i), 128);
        (void)sender.input(std::move(pkt));
    }
    stop = true;
    hammer.join();

    sender.close();
    receiver.close();
}

namespace
{

void send_report(int fd, const sockaddr_in &dst, const vstreamer::stream_link_report &rep)
{
    uint8_t wire[vstreamer::k_stream_header_len + vstreamer::k_stream_link_report_payload_len];
    vstreamer::stream_header hdr {};
    hdr.sequence_number = rep.report_seq;
    hdr.is_fec = false;
    hdr.is_stream_data = false;
    hdr.ext_len = 0;
    vstreamer::stream_header_write(wire, hdr);
    vstreamer::stream_link_report_encode_payload(rep, wire + vstreamer::k_stream_header_len,
                                                 vstreamer::k_stream_link_report_payload_len);
    sendto(fd, wire, sizeof(wire), 0, reinterpret_cast<const sockaddr *>(&dst), sizeof(dst));
}

vstreamer::stream_link_report make_report(uint32_t session, uint16_t seq, uint64_t udp_recv)
{
    vstreamer::stream_link_report r;
    r.session_id = session;
    r.report_seq = seq;
    r.timestamp_us = 1'000'000ULL;
    r.counters.udp_packet_received = udp_recv;
    return r;
}

}  // namespace

TEST(StreamSenderTest, PeerLinkReportSnapshot)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    vstreamer::stream_sender sender;
    ASSERT_EQ(0, cfg(sender, "stream", "127.0.0.1:9"));
    ASSERT_EQ(0, cfg(sender, "fec", "none"));
    ASSERT_EQ(0, sender.open());

    std::string local;
    ASSERT_EQ(0, sender.query("local", &local));
    EXPECT_FALSE(local.empty());

    char          host[64];
    int           bound_port = 0;
    ASSERT_EQ(2, std::sscanf(local.c_str(), "%63[^:]:%d", host, &bound_port));

    sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(bound_port));
    ASSERT_EQ(1, inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr));

    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(s, 0);

    const auto rep = make_report(42, 7, 99);
    send_report(s, dst, rep);

    for (int i = 0; i < 50; ++i)
    {
        const auto snap = sender.peer_link_snapshot();
        if (snap.have)
        {
            EXPECT_EQ(42U, snap.report.session_id);
            EXPECT_EQ(7U, snap.report.report_seq);
            EXPECT_EQ(99U, snap.report.counters.udp_packet_received);
            EXPECT_GE(snap.age_ms, 0);
            EXPECT_LT(snap.age_ms, 100);
            close(s);
            sender.close();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    FAIL() << "no peer report received";
    close(s);
    sender.close();
}

TEST(StreamSenderTest, LocalBindFixesSourcePort)
{
    const int free_port = ephemeral_udp_port();
    ASSERT_GT(free_port, 0);
    const int rx_port = ephemeral_udp_port();
    ASSERT_GT(rx_port, 0);

    const std::string local = "127.0.0.1:" + std::to_string(free_port);
    const std::string rx = "127.0.0.1:" + std::to_string(rx_port);

    vstreamer::stream_sender   sender;
    vstreamer::stream_receiver receiver;
    ASSERT_EQ(0, cfg(sender, "local", local));
    ASSERT_EQ(0, cfg(sender, "stream", rx));
    ASSERT_EQ(0, cfg(sender, "fec", "none"));
    ASSERT_EQ(0, cfg(receiver, "listen", rx));

    ASSERT_EQ(0, receiver.open());
    ASSERT_EQ(0, sender.open());
    ASSERT_EQ(0, sender.set_enabled(true, 0));

    auto pkt = vstreamer::test_pdu::make_stream_dgram(0, 64);
    ASSERT_EQ(0, sender.input(std::move(pkt)));

    std::string qlocal;
    ASSERT_EQ(0, sender.query("local", &qlocal));
    EXPECT_EQ(local, qlocal);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::string peer;
    while (peer.empty() && std::chrono::steady_clock::now() < deadline)
    {
        (void)receiver.query("telemetry_peer", &peer);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(local, peer);

    vstreamer::stream_sender sender2;
    ASSERT_EQ(0, cfg(sender2, "local", local));
    ASSERT_EQ(0, cfg(sender2, "stream", rx));
    ASSERT_EQ(0, cfg(sender2, "fec", "none"));
    EXPECT_LT(sender2.open(), 0);

    sender.close();
    receiver.close();
}

TEST(StreamSenderTest, PeerReportSeqGapAndReject)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    vstreamer::stream_sender sender;
    ASSERT_EQ(0, cfg(sender, "stream", "127.0.0.1:9"));
    ASSERT_EQ(0, cfg(sender, "fec", "none"));
    ASSERT_EQ(0, sender.open());

    std::string local;
    ASSERT_EQ(0, sender.query("local", &local));
    char host[64];
    int  bound_port = 0;
    ASSERT_EQ(2, std::sscanf(local.c_str(), "%63[^:]:%d", host, &bound_port));

    sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(bound_port));
    ASSERT_EQ(1, inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr));

    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(s, 0);

    send_report(s, dst, make_report(1, 0, 1));
    send_report(s, dst, make_report(1, 1, 2));
    send_report(s, dst, make_report(1, 4, 3));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto snap = sender.peer_link_snapshot();
    EXPECT_TRUE(snap.have);
    EXPECT_EQ(4U, snap.report.report_seq);
    EXPECT_EQ(2U, snap.reports_lost);

    send_report(s, dst, make_report(1, 4, 3));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    snap = sender.peer_link_snapshot();
    EXPECT_EQ(1U, snap.reports_rejected);

    close(s);
    sender.close();
}

TEST(StreamSenderTest, PeerReportNewSession)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    vstreamer::stream_sender sender;
    ASSERT_EQ(0, cfg(sender, "stream", "127.0.0.1:9"));
    ASSERT_EQ(0, cfg(sender, "fec", "none"));
    ASSERT_EQ(0, sender.open());

    std::string local;
    ASSERT_EQ(0, sender.query("local", &local));
    char host[64];
    int  bound_port = 0;
    ASSERT_EQ(2, std::sscanf(local.c_str(), "%63[^:]:%d", host, &bound_port));

    sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(bound_port));
    ASSERT_EQ(1, inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr));

    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(s, 0);

    send_report(s, dst, make_report(1, 0, 1));
    send_report(s, dst, make_report(1, 1, 2));
    send_report(s, dst, make_report(1, 4, 3));
    send_report(s, dst, make_report(2, 0, 9));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto snap = sender.peer_link_snapshot();
    EXPECT_EQ(2U, snap.report.session_id);
    EXPECT_EQ(0U, snap.report.report_seq);
    EXPECT_EQ(9U, snap.report.counters.udp_packet_received);
    EXPECT_EQ(2U, snap.reports_lost);

    close(s);
    sender.close();
}

TEST(StreamSenderTest, PeerReportRejectsGarbage)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    vstreamer::stream_sender sender;
    ASSERT_EQ(0, cfg(sender, "stream", "127.0.0.1:9"));
    ASSERT_EQ(0, cfg(sender, "fec", "none"));
    ASSERT_EQ(0, sender.open());

    std::string local;
    ASSERT_EQ(0, sender.query("local", &local));
    char host[64];
    int  bound_port = 0;
    ASSERT_EQ(2, std::sscanf(local.c_str(), "%63[^:]:%d", host, &bound_port));

    sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(bound_port));
    ASSERT_EQ(1, inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr));

    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(s, 0);

    send_report(s, dst, make_report(5, 0, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const auto before = sender.peer_link_snapshot();

    const char garbage[10] = {0};
    sendto(s, garbage, sizeof(garbage), 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
    uint8_t bad_magic[vstreamer::k_stream_header_len + vstreamer::k_stream_link_report_payload_len] {};
    sendto(s, bad_magic, sizeof(bad_magic), 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    const auto after = sender.peer_link_snapshot();
    EXPECT_EQ(before.report.session_id, after.report.session_id);
    EXPECT_EQ(before.report.report_seq, after.report.report_seq);
    EXPECT_EQ(2U, after.reports_rejected);

    close(s);
    sender.close();
}

TEST(StreamSenderTest, PeerReportAgeGrows)
{
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, cfg(sender, "stream", "127.0.0.1:9"));
    ASSERT_EQ(0, cfg(sender, "fec", "none"));
    ASSERT_EQ(0, sender.open());

    std::string local;
    ASSERT_EQ(0, sender.query("local", &local));
    char host[64];
    int  bound_port = 0;
    ASSERT_EQ(2, std::sscanf(local.c_str(), "%63[^:]:%d", host, &bound_port));

    sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(bound_port));
    ASSERT_EQ(1, inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr));

    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(s, 0);
    send_report(s, dst, make_report(3, 0, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(160));

    const auto snap = sender.peer_link_snapshot();
    EXPECT_GE(snap.age_ms, 150);

    close(s);
    sender.close();
}

TEST(StreamSenderTest, CloseJoinsTelemetryQuickly)
{
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, cfg(sender, "stream", "127.0.0.1:9"));
    ASSERT_EQ(0, cfg(sender, "fec", "none"));
    ASSERT_EQ(0, sender.open());
    const auto t0 = std::chrono::steady_clock::now();
    sender.close();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    EXPECT_LT(ms, 200);
}

/* Known keys answer before the first report / before open(); -ENOTSUP is only for
 * unknown keys. */
TEST(StreamSenderTest, PeerKeysAnswerBeforeFirstReport)
{
    vstreamer::stream_sender sender;
    std::string               val;
    ASSERT_EQ(0, sender.query("local", &val));
    EXPECT_EQ("0.0.0.0:0", val);
    ASSERT_EQ(0, sender.configure("stream", "127.0.0.1:9"));
    ASSERT_EQ(0, sender.open());
    for (const char *key : {"peer_udp_packet_received", "peer_fec_packet_received",
                            "peer_udp_gap_count", "peer_fec_gap_count", "peer_session",
                            "peer_reports_received", "peer_reports_lost", "peer_reports_rejected"})
    {
        EXPECT_EQ(0, sender.query(key, &val)) << key;
        EXPECT_EQ("0", val) << key;
    }
    ASSERT_EQ(0, sender.query("peer_report_age_ms", &val));
    EXPECT_EQ("-1", val);
    sender.close();
}

/* local = [host:]port, validated at configure(); port 0 = ephemeral. */
TEST(StreamSenderTest, LocalSpecForms)
{
    {
        vstreamer::stream_sender sender;
        EXPECT_EQ(-EINVAL, sender.configure("local", "junk"));
        EXPECT_EQ(-EINVAL, sender.configure("local", "127.0.0.1:70000"));
        EXPECT_EQ(-EINVAL, sender.configure("local", "127.0.0.1:"));
    }
    {
        const int port = ephemeral_udp_port();
        ASSERT_GT(port, 0);
        vstreamer::stream_sender sender;
        ASSERT_EQ(0, sender.configure("local", std::to_string(port)));
        ASSERT_EQ(0, sender.configure("stream", "127.0.0.1:9"));
        ASSERT_EQ(0, sender.open());
        std::string val;
        ASSERT_EQ(0, sender.query("local", &val));
        EXPECT_EQ("0.0.0.0:" + std::to_string(port), val);
        sender.close();
    }
    {
        vstreamer::stream_sender sender;
        ASSERT_EQ(0, sender.configure("local", "127.0.0.1:0"));
        ASSERT_EQ(0, sender.configure("stream", "127.0.0.1:9"));
        ASSERT_EQ(0, sender.open());
        std::string val;
        ASSERT_EQ(0, sender.query("local", &val));
        EXPECT_EQ(0U, val.rfind("127.0.0.1:", 0)) << val;
        EXPECT_NE("127.0.0.1:0", val);
        sender.close();
    }
}

namespace
{

uint8_t stream_flag_byte(const uint8_t *wire)
{
    return wire[2];
}

bool recv_one_udp(int fd, uint8_t *out, size_t cap, size_t *out_len, int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        pollfd pfd {fd, POLLIN, 0};
        if (poll(&pfd, 1, 20) <= 0)
        {
            continue;
        }
        const ssize_t n = recv(fd, out, cap, MSG_DONTWAIT);
        if (n > 0)
        {
            *out_len = static_cast<size_t>(n);
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(StreamSenderTest, RawInputBeforeOpenNotQueued)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    vstreamer::stream_sender   sender;
    vstreamer::stream_receiver receiver;
    const std::string          host_port = "127.0.0.1:" + std::to_string(port);
    ASSERT_EQ(0, cfg(sender, "stream", host_port));
    ASSERT_EQ(0, cfg(receiver, "listen", host_port));
    ASSERT_EQ(0, cfg(sender, "fec", "none"));

    ASSERT_EQ(0, receiver.open());
    ASSERT_EQ(0, sender.input(std::move(vstreamer::test_pdu::make_stream_dgram(0, 64))));
    ASSERT_EQ(0, sender.open());
    ASSERT_EQ(0, sender.set_enabled(true, 0));
    ASSERT_EQ(0, sender.input(std::move(vstreamer::test_pdu::make_stream_dgram(1, 64))));

    vstreamer::component_pdu out;
    const auto             deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int                    got = 0;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const int rc = receiver.output(out);
        if (0 == rc)
        {
            ++got;
        }
        else
        {
            ASSERT_EQ(-EAGAIN, rc);
        }
    }
    EXPECT_EQ(1, got);

    sender.close();
    receiver.close();
}

TEST(StreamSenderTest, MaxInputRawAndFecModes)
{
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, cfg(sender, "stream", "127.0.0.1:9"));
    ASSERT_EQ(0, cfg(sender, "max_datagram", "1472"));
    ASSERT_EQ(0, cfg(sender, "fec", "block"));
    ASSERT_EQ(0, cfg(sender, "fec_n", "12"));
    ASSERT_EQ(0, cfg(sender, "fec_k", "10"));
    ASSERT_EQ(0, sender.open());
    const size_t fec_in = query_size_t(sender, "max_input");
    EXPECT_EQ(1472U - 11U, fec_in);
    sender.close();

    vstreamer::stream_sender raw;
    ASSERT_EQ(0, cfg(raw, "stream", "127.0.0.1:9"));
    ASSERT_EQ(0, cfg(raw, "max_datagram", "1472"));
    ASSERT_EQ(0, cfg(raw, "fec", "none"));
    ASSERT_EQ(0, raw.open());
    const size_t raw_in = query_size_t(raw, "max_input");
    EXPECT_EQ(1472U - 4U, raw_in);
    raw.close();
}

TEST(StreamSenderTest, FecKnAccept31Reject32)
{
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, cfg(sender, "stream", "127.0.0.1:9"));
    ASSERT_EQ(0, cfg(sender, "fec", "block"));
    ASSERT_EQ(0, cfg(sender, "fec_n", "31"));
    ASSERT_EQ(0, cfg(sender, "fec_k", "31"));
    ASSERT_EQ(0, sender.open());
    EXPECT_LT(cfg(sender, "fec_n", "32"), 0);
    EXPECT_LT(cfg(sender, "fec_k", "32"), 0);
    sender.close();
}

TEST(StreamSenderTest, WireSequenceStampedOnFecAndRaw)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    const int sniff = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(sniff, 0);
    sockaddr_in bind_addr {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = htons(static_cast<uint16_t>(port));
    ASSERT_EQ(0, bind(sniff, reinterpret_cast<sockaddr *>(&bind_addr), sizeof(bind_addr)));

    vstreamer::stream_sender sender;
    const std::string        host_port = "127.0.0.1:" + std::to_string(port);
    ASSERT_EQ(0, cfg(sender, "stream", host_port));
    ASSERT_EQ(0, cfg(sender, "fec", "block"));
    ASSERT_EQ(0, cfg(sender, "fec_k", "1"));
    ASSERT_EQ(0, cfg(sender, "fec_n", "2"));
    ASSERT_EQ(0, sender.open());
    ASSERT_EQ(0, sender.set_enabled(true, 0));

    auto pkt = vstreamer::test_pdu::make_stream_dgram(0, 32);
    ASSERT_EQ(0, sender.input(std::move(pkt)));

    uint8_t wire[2048];
    size_t  wire_len = 0;
    ASSERT_TRUE(recv_one_udp(sniff, wire, sizeof(wire), &wire_len, 500));
    EXPECT_GE(wire_len, vstreamer::k_stream_header_len + vstreamer::rs_block_erasure::k_header_len);
    EXPECT_NE(0U, vstreamer::stream_header_sequence_be16(wire));
    EXPECT_NE(0U, stream_flag_byte(wire) & (1U << vstreamer::k_stream_flag_is_fec_shift));

    ASSERT_EQ(0, cfg(sender, "fec", "none"));
    ASSERT_EQ(0, sender.set_enabled(true, 0));
    ASSERT_EQ(0, sender.input(std::move(vstreamer::test_pdu::make_stream_dgram(1, 32))));
    bool saw_raw = false;
    for (int attempt = 0; attempt < 8 && !saw_raw; ++attempt)
    {
        if (!recv_one_udp(sniff, wire, sizeof(wire), &wire_len, 200))
        {
            break;
        }
        if (0 == (stream_flag_byte(wire) & (1U << vstreamer::k_stream_flag_is_fec_shift)))
        {
            saw_raw = true;
        }
    }
    EXPECT_TRUE(saw_raw);

    close(sniff);
    sender.close();
}

TEST(StreamSenderTest, RawPathDoesNotAdvanceSduBase)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    const int sniff = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(sniff, 0);
    sockaddr_in bind_addr {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port = htons(static_cast<uint16_t>(port));
    ASSERT_EQ(0, bind(sniff, reinterpret_cast<sockaddr *>(&bind_addr), sizeof(bind_addr)));

    vstreamer::stream_sender sender;
    const std::string        host_port = "127.0.0.1:" + std::to_string(port);
    ASSERT_EQ(0, cfg(sender, "stream", host_port));
    ASSERT_EQ(0, cfg(sender, "fec", "block"));
    ASSERT_EQ(0, cfg(sender, "fec_k", "1"));
    ASSERT_EQ(0, cfg(sender, "fec_n", "1"));
    ASSERT_EQ(0, sender.open());
    ASSERT_EQ(0, sender.set_enabled(true, 0));

    ASSERT_EQ(0, sender.input(std::move(vstreamer::test_pdu::make_stream_dgram(0, 32))));
    uint8_t wire[2048];
    size_t  wire_len = 0;
    ASSERT_TRUE(recv_one_udp(sniff, wire, sizeof(wire), &wire_len, 500));
    uint16_t base0 = 0;
    int      idx = 0;
    int      k = 0;
    int      n = 0;
    int      sdu_n = 0;
    ASSERT_TRUE(vstreamer::rs_block_erasure::unpack_header(
        wire + vstreamer::k_stream_header_len,
        wire_len - vstreamer::k_stream_header_len, &base0, &idx, &k, &n, &sdu_n));
    const int first_sdu_n = sdu_n;

    ASSERT_EQ(0, cfg(sender, "fec", "none"));
    for (int i = 0; i < 5; ++i)
    {
        ASSERT_EQ(0, sender.input(vstreamer::test_pdu::make_stream_dgram(static_cast<uint32_t>(i + 1), 32)));
        ASSERT_TRUE(recv_one_udp(sniff, wire, sizeof(wire), &wire_len, 200));
    }

    ASSERT_EQ(0, cfg(sender, "fec", "block"));
    ASSERT_EQ(0, sender.input(std::move(vstreamer::test_pdu::make_stream_dgram(99, 32))));
    ASSERT_TRUE(recv_one_udp(sniff, wire, sizeof(wire), &wire_len, 500));
    uint16_t base1 = 0;
    ASSERT_TRUE(vstreamer::rs_block_erasure::unpack_header(
        wire + vstreamer::k_stream_header_len,
        wire_len - vstreamer::k_stream_header_len, &base1, &idx, &k, &n, &sdu_n));
    EXPECT_EQ(static_cast<uint16_t>(base0 + static_cast<uint16_t>(first_sdu_n)), base1);

    close(sniff);
    sender.close();
}

TEST(StreamSenderTest, PortCapsAdvertisesStreamDgram)
{
    vstreamer::stream_sender sender;
    std::string              val;
    ASSERT_EQ(0, sender.query("inport-0.caps-0.sdu_type", &val));
    EXPECT_EQ("STREAM_DGRAM", val);
}

TEST(StreamSenderTest, PduInputEagainWhenQueueFull)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    vstreamer::stream_sender sender;
    const std::string        host_port = "127.0.0.1:" + std::to_string(port);
    ASSERT_EQ(0, cfg(sender, "stream", host_port));
    ASSERT_EQ(0, cfg(sender, "fec", "none"));
    ASSERT_EQ(0, cfg(sender, "max_kbps", "10"));
    ASSERT_EQ(0, cfg(sender, "queue_ms", "200"));
    ASSERT_EQ(0, sender.open());
    ASSERT_EQ(0, sender.set_enabled(true, 0));

    std::vector<uint8_t> payload(256, 0x5A);
    bool                 saw_eagain = false;
    for (int i = 0; i < 800; ++i)
    {
        vstreamer::component_pdu pdu;
        pdu.sdu_type = vstreamer::sdu_type_e::STREAM_DGRAM;
        pdu.port = 0;
        pdu.sdu = vstreamer::shared_sized_buffer::copy_from(payload.data(), payload.size());
        const int rc = sender.input(std::move(pdu));
        if (-EAGAIN == rc)
        {
            saw_eagain = true;
            break;
        }
        ASSERT_EQ(0, rc);
    }
    EXPECT_TRUE(saw_eagain);

    std::string dropped_before;
    ASSERT_EQ(0, sender.query("dropped", &dropped_before));
    const uint64_t dropped0 = std::strtoull(dropped_before.c_str(), nullptr, 10);
    auto overflow_pkt = vstreamer::test_pdu::make_stream_dgram(9999, 64);
    EXPECT_EQ(-EAGAIN, sender.input(std::move(overflow_pkt)));
    std::string dropped_after;
    ASSERT_EQ(0, sender.query("dropped", &dropped_after));
    EXPECT_EQ(dropped0, std::strtoull(dropped_after.c_str(), nullptr, 10));

    sender.close();
}

/* With FEC on, an SDU that closes a block produces its data shard plus all of the block's parity.
 * A full queue must refuse the whole SDU before it enters the FEC block; queueing some shards and
 * then returning -EAGAIN sends a block without part of its data or parity while the caller drops
 * the datagram as not taken. */
TEST(StreamSenderTest, PduInputFecQueueFullRefusesWholeSdu)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    vstreamer::stream_sender sender;
    const std::string        host_port = "127.0.0.1:" + std::to_string(port);
    ASSERT_EQ(0, cfg(sender, "stream", host_port));
    ASSERT_EQ(0, cfg(sender, "fec_k", "2"));
    ASSERT_EQ(0, cfg(sender, "fec_n", "12"));
    ASSERT_EQ(0, cfg(sender, "max_kbps", "10"));
    ASSERT_EQ(0, cfg(sender, "queue_ms", "100"));
    ASSERT_EQ(0, sender.open());
    ASSERT_EQ(0, sender.set_enabled(true, 0));

    int  rejects = 0;
    for (int i = 0; i < 400 && rejects < 20; ++i)
    {
        const size_t before = query_size_t(sender, "queue_bytes");
        auto         pkt = vstreamer::test_pdu::make_stream_dgram(static_cast<uint32_t>(i), 1000);
        const int    rc = sender.input(std::move(pkt));
        const size_t after = query_size_t(sender, "queue_bytes");
        if (-EAGAIN == rc)
        {
            rejects++;
            EXPECT_LE(after, before) << "refused SDU " << i << " still queued packets";
            continue;
        }
        ASSERT_EQ(0, rc);
    }
    EXPECT_GT(rejects, 0);

    std::string rejects_s;
    ASSERT_EQ(0, sender.query("queue_full_rejects", &rejects_s));
    EXPECT_EQ(static_cast<uint64_t>(rejects), std::strtoull(rejects_s.c_str(), nullptr, 10));

    sender.close();
}
