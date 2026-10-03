#include "components/stream_receiver.hpp"
#include "components/stream_sender.hpp"

#include "core/component.hpp"
#include "core/data_packet.hpp"
#include "core/shared_sized_buffer.hpp"
#include "core/stream_telemetry.hpp"

#include <gtest/gtest.h>

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

vstreamer::data_packet make_sock_packet(uint32_t counter, size_t payload_bytes)
{
    std::vector<uint8_t> storage(payload_bytes);
    storage[0] = static_cast<uint8_t>((counter >> 24) & 0xFF);
    storage[1] = static_cast<uint8_t>((counter >> 16) & 0xFF);
    storage[2] = static_cast<uint8_t>((counter >> 8) & 0xFF);
    storage[3] = static_cast<uint8_t>(counter & 0xFF);

    auto sd = std::make_unique<vstreamer::sock_data>();
    sd->pts = static_cast<int64_t>(counter);
    sd->buf = vstreamer::shared_sized_buffer::copy_from(storage.data(), storage.size());

    vstreamer::data_packet pkt;
    pkt.reset(std::move(sd));
    return pkt;
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

TEST(StreamSenderTest, QueueByteCapEvictsOldestUnderPacing)
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
    constexpr int k_packets = 6000;
    constexpr size_t k_payload = 128;

    for (int i = 0; i < k_packets; ++i)
    {
        const auto pkt = make_sock_packet(static_cast<uint32_t>(i), k_payload);
        ASSERT_EQ(0, sender.input(0, pkt));
        const size_t qb = query_size_t(sender, "queue_bytes");
        EXPECT_LE(qb, limit);
    }

    std::string dropped_s;
    ASSERT_EQ(0, sender.query("dropped", &dropped_s));
    const uint64_t dropped = std::strtoull(dropped_s.c_str(), nullptr, 10);
    EXPECT_GT(dropped, 0ULL);

    const uint64_t sent = sender.wire_pkts_sent_counter().load();
    EXPECT_GE(sent + dropped, static_cast<uint64_t>(k_packets - 500));

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
        const auto pkt = make_sock_packet(static_cast<uint32_t>(i), 128);
        (void)sender.input(0, pkt);
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
    uint8_t wire[vstreamer::k_stream_link_report_len];
    vstreamer::stream_link_report_encode(rep, wire);
    sendto(fd, wire, sizeof(wire), 0, reinterpret_cast<const sockaddr *>(&dst), sizeof(dst));
}

vstreamer::stream_link_report make_report(uint32_t session, uint32_t seq, uint64_t udp_recv)
{
    vstreamer::stream_link_report r;
    r.session_id = session;
    r.report_seq = seq;
    r.interval_ms = 100;
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

    const auto pkt = make_sock_packet(0, 64);
    ASSERT_EQ(0, sender.input(0, pkt));

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
    uint8_t bad_magic[vstreamer::k_stream_link_report_len] {};
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

/* TT-R4: known keys answer before the first report / before open(); -ENOTSUP is only for
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
