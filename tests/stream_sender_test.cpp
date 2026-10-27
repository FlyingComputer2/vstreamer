#include "components/stream_sender.hpp"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <bfc/shared_sized_buffer.hpp>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "components/stream_receiver.hpp"
#include "core/component.hpp"
#include "core/component_pdu.hpp"
#include "core/default_reactor_host.hpp"
#include "core/rs_block_erasure.hpp"
#include "core/stream_header.hpp"
#include "core/stream_telemetry.hpp"
#include "stream_query_test_helpers.hpp"
#include "test_pdu_helpers.hpp"

namespace
{

int cfg(vstreamer::component &c, std::string_view key, std::string_view value)
{
    return c.configure(key, value);
}

/* Opens sender to a loopback sink, sends one datagram, returns the bound source address for
 * reverse telemetry injection. */
bool open_sender_with_reply_addr(vstreamer::stream_sender &sender, sockaddr_in *reply_dst)
{
    if (nullptr == reply_dst)
    {
        return false;
    }
    const int rx = socket(AF_INET, SOCK_DGRAM, 0);
    if (rx < 0)
    {
        return false;
    }
    sockaddr_in bind_addr {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(rx, reinterpret_cast<sockaddr *>(&bind_addr), sizeof(bind_addr)) < 0)
    {
        close(rx);
        return false;
    }
    socklen_t len = sizeof(bind_addr);
    if (getsockname(rx, reinterpret_cast<sockaddr *>(&bind_addr), &len) < 0)
    {
        close(rx);
        return false;
    }
    const std::string stream =
        "127.0.0.1:" + std::to_string(ntohs(bind_addr.sin_port));
    if (0 != cfg(sender, "udp_transport", stream) || 0 != cfg(sender, "fec", "none") ||
        0 != sender.open())
    {
        close(rx);
        return false;
    }
    if (0 != sender.input(vstreamer::test_pdu::make_stream_dgram(0, 16)))
    {
        sender.close();
        close(rx);
        return false;
    }
    uint8_t buf[256];
    sockaddr_in from {};
    from.sin_family = AF_INET;
    len = sizeof(from);
    pollfd pfd {rx, POLLIN, 0};
    bool   got = false;
    for (int i = 0; i < 50; ++i)
    {
        if (poll(&pfd, 1, 20) > 0 &&
            recvfrom(rx, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &len) > 0)
        {
            got = true;
            break;
        }
    }
    close(rx);
    if (!got)
    {
        sender.close();
        return false;
    }
    *reply_dst = from;
    return true;
}

}  // namespace

TEST(StreamSenderTest, OpenWithHostnameDoesNotHang)
{
    const int port = 5091;
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    const std::string        stream = "localhost:" + std::to_string(port);
    ASSERT_EQ(0, cfg(sender, "udp_transport", stream));
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
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    ASSERT_EQ(0, cfg(sender, "udp_transport", "bogus.invalid:5000"));
    auto fut = std::async(std::launch::async, [&sender]() { return sender.open(); });
    EXPECT_EQ(std::future_status::ready, fut.wait_for(std::chrono::seconds(2)));
    ASSERT_EQ(std::future_status::ready, fut.wait_for(std::chrono::seconds(0)));
    EXPECT_LT(fut.get(), 0);
}

TEST(StreamSenderTest, ConfigureLongValueRejected)
{
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    const std::string        long_ms(200, '9');
    EXPECT_LT(cfg(sender, "fec_spread_ms", long_ms), 0);
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

/* No rate control: the send queue is a fixed packet capacity, with no time window or rate-derived
 * byte limit to configure or report. */
TEST(StreamSenderTest, NoRateDerivedQueueKeys)
{
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    EXPECT_LT(cfg(sender, "queue_ms", "100"), 0);
    std::string v;
    EXPECT_LT(sender.query("queue_ms", &v), 0);
    EXPECT_LT(sender.query("queue_byte_limit", &v), 0);
    EXPECT_LT(sender.query("in_rate", &v), 0);
}

/* A keyframe's data and parity are enqueued at once. None of it may be evicted, or the
 * receiver gets the missing data shards back from parity only after later packets. */
TEST(StreamSenderTest, KeyframeBurstWithFecNotEvicted)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    const std::string        host_port = "127.0.0.1:" + std::to_string(port);
    ASSERT_EQ(0, cfg(sender, "udp_transport", host_port));
    ASSERT_EQ(0, cfg(sender, "fec_n", "15"));
    ASSERT_EQ(0, cfg(sender, "fec_k", "8"));
    ASSERT_EQ(0, sender.open());

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
    std::string evicted_s;
    ASSERT_EQ(0, sender.query("queue_evicted", &evicted_s));
    EXPECT_EQ(0ULL, std::strtoull(evicted_s.c_str(), nullptr, 10));

    sender.close();
}

/* The SDU that completes a block releases its data shard and all of the block's parity at once.
 * The room check must reserve them all, or the forced enqueue pushes the queue past its packet
 * capacity. Spread parity is what backs the queue up here. */
TEST(StreamSenderTest, QueuePacketCapHoldsWholeBlocksWhenSpreadBacksQueue)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    const std::string        host_port = "127.0.0.1:" + std::to_string(port);
    ASSERT_EQ(0, cfg(sender, "udp_transport", host_port));
    ASSERT_EQ(0, cfg(sender, "fec_n", "31"));
    ASSERT_EQ(0, cfg(sender, "fec_k", "21"));
    ASSERT_EQ(0, cfg(sender, "fec_spread_ms", "250"));
    ASSERT_EQ(0, cfg(sender, "telemetry", "off"));

    ASSERT_EQ(0, sender.open());

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
        EXPECT_LE(query_size_t(sender, "queue_packets"), 1024U);
    }
    EXPECT_TRUE(saw_eagain);

    std::string dropped_s;
    ASSERT_EQ(0, sender.query("dropped", &dropped_s));
    EXPECT_EQ(0ULL, std::strtoull(dropped_s.c_str(), nullptr, 10));
    std::string evicted_s;
    ASSERT_EQ(0, sender.query("queue_evicted", &evicted_s));
    EXPECT_EQ(0ULL, std::strtoull(evicted_s.c_str(), nullptr, 10));

    sender.close();
}

TEST(StreamSenderTest, FecReconfigureWhileSendingSurvivesExtremes)
{
    /* M-plane misconfiguration must never crash the sender: k/n (1/31 is a 31x amplification),
     * and spread change from another thread while full-size PDUs stream through. */
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    const std::string        host_port = "127.0.0.1:" + std::to_string(port);
    ASSERT_EQ(0, cfg(sender, "udp_transport", host_port));
    ASSERT_EQ(0, cfg(sender, "fec_n", "12"));
    ASSERT_EQ(0, cfg(sender, "fec_k", "10"));
    ASSERT_EQ(0, sender.open());

    std::atomic<bool> stop {false};
    std::thread       hammer([&]() {
        const char *steps[][2] = {
            {"fec_n", "31"},          {"fec_k", "10"},        {"fec_k", "1"},
            {"fec_spread_ms", "250"}, {"fec_k", "2"},
            {"fec_n", "2"},           {"fec_k", "2"},         {"fec_n", "31"},
            {"fec_k", "1"},           {"fec", "none"},        {"fec_n", "31"},
            {"fec_k", "31"},          {"fec_spread_ms", "0"},
            {"fec_k", "21"},
        };
        while (!stop.load())
        {
            for (const auto &st : steps)
            {
                (void)cfg(sender, st[0], st[1]);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    });

    for (int i = 0; i < 4000; ++i)
    {
        auto pkt = vstreamer::test_pdu::make_stream_dgram(static_cast<uint32_t>(i), 1200);
        (void)sender.input(std::move(pkt));
    }
    stop = true;
    hammer.join();
    sender.close();
}

TEST(StreamSenderTest, LoweringFecSpreadReleasesQueuedShards)
{
    /* A long fec_spread_ms parks shards in the send queue for that long; lowering it again must
     * pull them in instead of leaving the stream stalled until the old release times. */
    const int rx = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(rx, 0);
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(0, bind(rx, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)));
    socklen_t alen = sizeof(addr);
    ASSERT_EQ(0, getsockname(rx, reinterpret_cast<sockaddr *>(&addr), &alen));

    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    ASSERT_EQ(0, cfg(sender, "udp_transport", "127.0.0.1:" + std::to_string(ntohs(addr.sin_port))));
    ASSERT_EQ(0, cfg(sender, "fec_k", "2"));
    ASSERT_EQ(0, cfg(sender, "fec_n", "4"));
    ASSERT_EQ(0, cfg(sender, "fec_spread_ms", "250"));
    ASSERT_EQ(0, sender.open());
    for (int i = 0; i < 8; ++i)
    {
        ASSERT_EQ(0, sender.input(vstreamer::test_pdu::make_stream_dgram(static_cast<uint32_t>(i), 200)));
    }

    const auto drain = [rx](int ms) {
        int got = 0;
        pollfd pfd {rx, POLLIN, 0};
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end)
        {
            if (poll(&pfd, 1, 10) > 0)
            {
                uint8_t b[2048];
                got += recv(rx, b, sizeof(b), 0) > 0 ? 1 : 0;
            }
        }
        return got;
    };
    const int early = drain(30);
    EXPECT_LT(early, 16) << "spread 250 ms should hold most shards back";
    ASSERT_EQ(0, cfg(sender, "fec_spread_ms", "0"));
    EXPECT_EQ(16, early + drain(500)) << "4 blocks x n=4 shards";

    sender.close();
    close(rx);
}

TEST(StreamSenderTest, RemovedEgressKnobsConfigureRejected)
{
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    EXPECT_EQ(-ENOTSUP, cfg(sender, "max_kbps", "1000"));
    EXPECT_EQ(-ENOTSUP, cfg(sender, "max_datagram", "600"));
    EXPECT_EQ(-ENOTSUP, cfg(sender, "mtu", "1400"));
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
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    sockaddr_in              dst {};
    ASSERT_TRUE(open_sender_with_reply_addr(sender, &dst));

    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(s, 0);

    const auto rep = make_report(42, 7, 99);
    send_report(s, dst, rep);

    for (int i = 0; i < 50; ++i)
    {
        const auto snap = vstreamer::test_query::peer(sender);
        if (snap.have)
        {
            EXPECT_EQ(42U, snap.report.session_id);
            EXPECT_EQ(1U, snap.reports_received);
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

TEST(StreamSenderTest, PeerReportSeqGapAndReject)
{
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    sockaddr_in              dst {};
    ASSERT_TRUE(open_sender_with_reply_addr(sender, &dst));

    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(s, 0);

    send_report(s, dst, make_report(1, 0, 1));
    send_report(s, dst, make_report(1, 1, 2));
    send_report(s, dst, make_report(1, 4, 3));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto snap = vstreamer::test_query::peer(sender);
    EXPECT_TRUE(snap.have);
    EXPECT_EQ(3U, snap.report.counters.udp_packet_received);
    EXPECT_EQ(2U, snap.reports_lost);

    send_report(s, dst, make_report(1, 4, 3));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    snap = vstreamer::test_query::peer(sender);
    EXPECT_EQ(1U, snap.reports_rejected);

    close(s);
    sender.close();
}

TEST(StreamSenderTest, PeerReportNewSession)
{
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    sockaddr_in              dst {};
    ASSERT_TRUE(open_sender_with_reply_addr(sender, &dst));

    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(s, 0);

    send_report(s, dst, make_report(1, 0, 1));
    send_report(s, dst, make_report(1, 1, 2));
    send_report(s, dst, make_report(1, 4, 3));
    send_report(s, dst, make_report(2, 0, 9));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const auto snap = vstreamer::test_query::peer(sender);
    EXPECT_EQ(2U, snap.report.session_id);
    EXPECT_EQ(9U, snap.report.counters.udp_packet_received);
    EXPECT_EQ(2U, snap.reports_lost);

    close(s);
    sender.close();
}

TEST(StreamSenderTest, PeerReportRejectsGarbage)
{
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    sockaddr_in              dst {};
    ASSERT_TRUE(open_sender_with_reply_addr(sender, &dst));

    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(s, 0);

    send_report(s, dst, make_report(5, 0, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const auto before = vstreamer::test_query::peer(sender);

    const char garbage[10] = {0};
    sendto(s, garbage, sizeof(garbage), 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
    uint8_t bad_magic[vstreamer::k_stream_header_len + vstreamer::k_stream_link_report_payload_len] {};
    sendto(s, bad_magic, sizeof(bad_magic), 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    const auto after = vstreamer::test_query::peer(sender);
    EXPECT_EQ(before.report.session_id, after.report.session_id);
    EXPECT_EQ(before.report.counters.udp_packet_received,
              after.report.counters.udp_packet_received);
    EXPECT_EQ(2U, after.reports_rejected);

    close(s);
    sender.close();
}

TEST(StreamSenderTest, PeerReportAgeGrows)
{
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    sockaddr_in              dst {};
    ASSERT_TRUE(open_sender_with_reply_addr(sender, &dst));

    const int s = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(s, 0);
    send_report(s, dst, make_report(3, 0, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(160));

    const auto snap = vstreamer::test_query::peer(sender);
    EXPECT_GE(snap.age_ms, 150);

    close(s);
    sender.close();
}

TEST(StreamSenderTest, CloseJoinsTelemetryQuickly)
{
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    ASSERT_EQ(0, cfg(sender, "udp_transport", "127.0.0.1:9"));
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
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    std::string               val;
    ASSERT_EQ(0, sender.configure("udp_transport", "127.0.0.1:9"));
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

namespace
{


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

    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender   sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    vstreamer::stream_receiver receiver;
    const std::string          host_port = "127.0.0.1:" + std::to_string(port);
    ASSERT_EQ(0, cfg(sender, "udp_transport", host_port));
    ASSERT_EQ(0, cfg(receiver, "listen", host_port));
    ASSERT_EQ(0, cfg(sender, "fec", "none"));

    ASSERT_EQ(0, receiver.open());
    ASSERT_EQ(0, sender.input(std::move(vstreamer::test_pdu::make_stream_dgram(0, 64))));
    ASSERT_EQ(0, sender.open());
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
    constexpr size_t k_dg = static_cast<size_t>(vstreamer::stream_sender::k_wire_datagram_bytes);

    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    ASSERT_EQ(0, cfg(sender, "udp_transport", "127.0.0.1:9"));
    ASSERT_EQ(0, cfg(sender, "fec", "block"));
    ASSERT_EQ(0, cfg(sender, "fec_n", "12"));
    ASSERT_EQ(0, cfg(sender, "fec_k", "10"));
    ASSERT_EQ(0, sender.open());
    /* Parity header (4) and its coded length (2) bound every mode alike. */
    const size_t fec_in = query_size_t(sender, "max_input");
    EXPECT_EQ(k_dg - 6U, fec_in);
    sender.close();

    vstreamer::default_reactor_host raw_reactor;
    vstreamer::stream_sender raw;
    ASSERT_EQ(0, raw.configure("reactor", raw_reactor.name()));
    ASSERT_EQ(0, cfg(raw, "udp_transport", "127.0.0.1:9"));
    ASSERT_EQ(0, cfg(raw, "fec", "none"));
    ASSERT_EQ(0, raw.open());
    const size_t raw_in = query_size_t(raw, "max_input");
    EXPECT_EQ(k_dg - 6U, raw_in);
    raw.close();
}

TEST(StreamSenderTest, FecKnAccept31Reject32)
{
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    ASSERT_EQ(0, cfg(sender, "udp_transport", "127.0.0.1:9"));
    ASSERT_EQ(0, cfg(sender, "fec", "block"));
    ASSERT_EQ(0, cfg(sender, "fec_n", "31"));
    ASSERT_EQ(0, cfg(sender, "fec_k", "31"));
    ASSERT_EQ(0, sender.open());
    EXPECT_LT(cfg(sender, "fec_n", "32"), 0);
    EXPECT_LT(cfg(sender, "fec_k", "32"), 0);
    sender.close();
}

/* Data shards carry one running sequence whatever the FEC mode; parity names its block's first
 * data shard and takes no sequence of its own. */
TEST(StreamSenderTest, DataSequenceRunsAcrossFecModes)
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

    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    const std::string        host_port = "127.0.0.1:" + std::to_string(port);
    ASSERT_EQ(0, cfg(sender, "udp_transport", host_port));
    ASSERT_EQ(0, cfg(sender, "telemetry", "off"));
    ASSERT_EQ(0, cfg(sender, "fec", "block"));
    ASSERT_EQ(0, cfg(sender, "fec_k", "1"));
    ASSERT_EQ(0, cfg(sender, "fec_n", "2"));
    ASSERT_EQ(0, sender.open());

    uint8_t                                wire[2048];
    size_t                                 wire_len = 0;
    vstreamer::rs_block_erasure::shard_header h;
    const auto next = [&]() {
        EXPECT_TRUE(recv_one_udp(sniff, wire, sizeof(wire), &wire_len, 500));
        EXPECT_TRUE(vstreamer::rs_block_erasure::parse_header(wire, wire_len, &h));
    };

    ASSERT_EQ(0, sender.input(vstreamer::test_pdu::make_stream_dgram(0, 32)));
    next();
    ASSERT_TRUE(h.is_data);
    const uint16_t seq0 = h.sequence_number;
    EXPECT_EQ(wire_len, 3U + 32U);
    next();
    ASSERT_FALSE(h.is_data);
    EXPECT_EQ(seq0, h.sequence_number);
    EXPECT_EQ(1, h.k_total);
    EXPECT_EQ(1, h.n);

    ASSERT_EQ(0, cfg(sender, "fec", "none"));
    for (uint16_t i = 1; i <= 5; ++i)
    {
        ASSERT_EQ(0, sender.input(vstreamer::test_pdu::make_stream_dgram(i, 32)));
        next();
        ASSERT_TRUE(h.is_data);
        EXPECT_EQ(static_cast<uint16_t>(seq0 + i), h.sequence_number);
    }

    ASSERT_EQ(0, cfg(sender, "fec", "block"));
    ASSERT_EQ(0, sender.input(vstreamer::test_pdu::make_stream_dgram(99, 32)));
    next();
    ASSERT_TRUE(h.is_data);
    EXPECT_EQ(static_cast<uint16_t>(seq0 + 6), h.sequence_number);

    close(sniff);
    sender.close();
}

TEST(StreamSenderTest, PortCapsAdvertisesStreamDgram)
{
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    std::string              val;
    ASSERT_EQ(0, sender.query("inport-0.caps-0.sdu_type", &val));
    EXPECT_EQ("STREAM_DGRAM", val);
    ASSERT_EQ(0, sender.query("inport-0.caps-1.sdu_type", &val));
    EXPECT_EQ("RTP", val);
}

/* rtp_h264_pay output can be linked straight to the sender, without relabelling. */
TEST(StreamSenderTest, AcceptsRtpPdus)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    ASSERT_EQ(0, cfg(sender, "udp_transport", "127.0.0.1:" + std::to_string(port)));
    ASSERT_EQ(0, sender.open());
    auto pdu = vstreamer::test_pdu::make_stream_dgram(1, 64);
    pdu.sdu_type = vstreamer::sdu_type_e::RTP;
    EXPECT_EQ(0, sender.input(std::move(pdu)));
    auto other = vstreamer::test_pdu::make_stream_dgram(2, 64);
    other.sdu_type = vstreamer::sdu_type_e::H264_AU;
    EXPECT_EQ(-EINVAL, sender.input(std::move(other)));
    sender.close();
}

TEST(StreamSenderTest, PduInputEagainWhenQueueFull)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    const std::string        host_port = "127.0.0.1:" + std::to_string(port);
    ASSERT_EQ(0, cfg(sender, "udp_transport", host_port));
    ASSERT_EQ(0, cfg(sender, "fec_k", "2"));
    ASSERT_EQ(0, cfg(sender, "fec_n", "12"));
    ASSERT_EQ(0, cfg(sender, "fec_spread_ms", "250"));
    ASSERT_EQ(0, cfg(sender, "telemetry", "off"));
    ASSERT_EQ(0, sender.open());

    std::vector<uint8_t> payload(1000, 0x5A);
    bool                 saw_eagain = false;
    for (int i = 0; i < 400; ++i)
    {
        vstreamer::component_pdu pdu;
        pdu.sdu_type = vstreamer::sdu_type_e::STREAM_DGRAM;
        pdu.port = 0;
        pdu.sdu = bfc::shared_sized_buffer::copy_from(payload.data(), payload.size());
        const int rc = sender.input(std::move(pdu));
        if (-EAGAIN == rc)
        {
            saw_eagain = true;
            break;
        }
        ASSERT_EQ(0, rc);
    }
    EXPECT_TRUE(saw_eagain);

    /* The send thread may drain a slot between the EAGAIN above and this push; retry until one is
     * refused. */
    bool refused = false;
    for (int attempt = 0; attempt < 50 && !refused; ++attempt)
    {
        std::string dropped_before;
        ASSERT_EQ(0, sender.query("dropped", &dropped_before));
        const uint64_t dropped0 = std::strtoull(dropped_before.c_str(), nullptr, 10);
        auto overflow_pkt = vstreamer::test_pdu::make_stream_dgram(9999, 64);
        const int rc = sender.input(std::move(overflow_pkt));
        if (0 == rc)
        {
            continue;
        }
        ASSERT_EQ(-EAGAIN, rc);
        refused = true;
        std::string dropped_after;
        ASSERT_EQ(0, sender.query("dropped", &dropped_after));
        EXPECT_EQ(dropped0, std::strtoull(dropped_after.c_str(), nullptr, 10));
    }
    EXPECT_TRUE(refused);

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

    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    const std::string        host_port = "127.0.0.1:" + std::to_string(port);
    ASSERT_EQ(0, cfg(sender, "udp_transport", host_port));
    ASSERT_EQ(0, cfg(sender, "fec_k", "2"));
    ASSERT_EQ(0, cfg(sender, "fec_n", "12"));
    ASSERT_EQ(0, cfg(sender, "fec_spread_ms", "250"));
    ASSERT_EQ(0, cfg(sender, "telemetry", "off"));
    ASSERT_EQ(0, sender.open());

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

TEST(StreamSenderTest, NamedReactorMissingAndConfigureContract)
{
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    std::string name;
    ASSERT_EQ(0, sender.query("reactor", &name));
    EXPECT_EQ(sender_reactor.name(), name);
    ASSERT_EQ(0, cfg(sender, "udp_transport", "127.0.0.1:5091"));
    ASSERT_EQ(0, cfg(sender, "reactor", ""));
    EXPECT_EQ(-EINVAL, sender.open());
    ASSERT_EQ(0, cfg(sender, "reactor", "missing-sender-test-reactor"));
    ASSERT_EQ(0, sender.query("reactor", &name));
    EXPECT_EQ(name, "missing-sender-test-reactor");
    ASSERT_EQ(0, cfg(sender, "udp_transport", "127.0.0.1:5091"));
    EXPECT_EQ(-ENOENT, sender.open());
    // A failed lookup can be corrected and retried.
    ASSERT_EQ(0, cfg(sender, "reactor", sender_reactor.name()));
    ASSERT_EQ(0, sender.open());
    EXPECT_EQ(-EBUSY, cfg(sender, "reactor", "other"));
    sender.close();
}

namespace
{
struct registered_sender_reactor
{
    std::shared_ptr<vstreamer::default_reactor> reactor =
        std::make_shared<vstreamer::default_reactor>();
    std::thread worker;
    registered_sender_reactor()
    {
        vstreamer::default_reactor_registry::get_instance().add("sender-test-reactor", reactor);
        auto started = std::make_shared<std::promise<void>>();
        auto ready = started->get_future();
        reactor->wake_up([started] { started->set_value(); });
        worker = std::thread([this] { reactor->run(); });
        ready.wait();
    }
    ~registered_sender_reactor()
    {
        reactor->stop();
        worker.join();
        vstreamer::default_reactor_registry::get_instance().rem("sender-test-reactor");
    }
};
}

TEST(StreamSenderTest, NamedReactorReceivesTelemetryAndSurvivesSenderRestart)
{
    registered_sender_reactor host;
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    ASSERT_EQ(0, cfg(sender, "reactor", "sender-test-reactor"));
    for (int cycle = 0; cycle < 2; ++cycle)
    {
        sockaddr_in dst {};
        ASSERT_TRUE(open_sender_with_reply_addr(sender, &dst));
        const int fd = socket(AF_INET, SOCK_DGRAM, 0);
        ASSERT_GE(fd, 0);
        send_report(fd, dst, make_report(42, 7, 99));
        close(fd);
        bool received = false;
        for (int i = 0; i < 100; ++i)
        {
            const auto snap = vstreamer::test_query::peer(sender);
            if (snap.have)
            {
                EXPECT_EQ(99u, snap.report.counters.udp_packet_received);
                received = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_TRUE(received);
        sender.close();
        auto alive = std::make_shared<std::promise<void>>();
        auto ready = alive->get_future();
        host.reactor->wake_up([alive] { alive->set_value(); });
        EXPECT_EQ(std::future_status::ready, ready.wait_for(std::chrono::seconds(1)));
    }
}

TEST(StreamSenderTest, TelemetryOffStillRequiresNamedReactor)
{
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    ASSERT_EQ(0, cfg(sender, "udp_transport", "127.0.0.1:5091"));
    ASSERT_EQ(0, cfg(sender, "reactor", "missing-sender-test-reactor"));
    ASSERT_EQ(0, cfg(sender, "telemetry", "off"));
    EXPECT_EQ(-ENOENT, sender.open());
    ASSERT_EQ(0, cfg(sender, "reactor", ""));
    EXPECT_EQ(-EINVAL, sender.open());
    ASSERT_EQ(0, cfg(sender, "reactor", sender_reactor.name()));
    EXPECT_EQ(0, sender.open());
    sender.close();
}
