#include "components/stream_sender.hpp"
#include "core/config_keys.hpp"

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
#include "core/stream_media_header.hpp"
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
    if (0 != cfg(sender, "udp_transport", stream) || 0 != sender.open())
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

/* FEC is the link's job: the sender takes none of the old FEC keys. */
TEST(StreamSenderTest, FecKeysRejected)
{
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    for (const char *key : {"fec", "fec_k", "fec_n", "fec_spread_ms"})
    {
        EXPECT_EQ(-ENOTSUP, cfg(sender, key, "1")) << key;
    }
    uint64_t v = 0;
    for (const char *key : {"fec_k", "fec_n", "fec_spread_ms", "fec_push_us", "fec_kernel_us"})
    {
        EXPECT_EQ(-ENOTSUP, sender.query(vstreamer::query_key(key), &v)) << key;
    }
    std::string mode;
    EXPECT_EQ(-ENOTSUP, sender.query("fec_mode", &mode));
    EXPECT_EQ(-ENOTSUP, sender.configure(vstreamer::query_key("bitrate"), 1000));
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

TEST(StreamSenderTest, TypedQueryMatchesStringAndRejectsWrongType)
{
    vstreamer::stream_sender sender;

    for (const char *key : {"oversized", "max_datagram", "max_input", "dropped",
                            "queue_packets", "peer_reports_received", "peer_session"})
    {
        uint64_t typed = 0;
        ASSERT_EQ(0, sender.query(vstreamer::query_key(key), &typed)) << key;
        std::string str;
        ASSERT_EQ(0, sender.query(key, &str)) << key;
        EXPECT_EQ(std::to_string(typed), str) << key;
        int64_t i = 0;
        EXPECT_EQ(-ENOTSUP, sender.query(vstreamer::query_key(key), &i)) << key;
        double d = 0.0;
        EXPECT_EQ(-ENOTSUP, sender.query(vstreamer::query_key(key), &d)) << key;
    }

    int64_t age = 0;
    ASSERT_EQ(0, sender.query(vstreamer::query_key("peer_report_age_ms"), &age));
    std::string age_s;
    ASSERT_EQ(0, sender.query("peer_report_age_ms", &age_s));
    EXPECT_EQ(std::to_string(age), age_s);

    double late = -1.0;
    ASSERT_EQ(0, sender.query(vstreamer::query_key("send_late_max_ms"), &late));
    EXPECT_EQ(0.0, late);
    std::string late_s;
    ASSERT_EQ(0, sender.query("send_late_max_ms", &late_s));
    EXPECT_EQ("0.0", late_s);

    uint64_t u = 0;
    EXPECT_EQ(-ENOTSUP, sender.query(vstreamer::query_key("no_such_key"), &u));
    EXPECT_EQ(-EINVAL,
              sender.query(vstreamer::query_key("dropped"), static_cast<uint64_t *>(nullptr)));
}

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

TEST(StreamSenderTest, MaxInputIsDatagramLessMediaHeader)
{
    constexpr size_t k_dg = static_cast<size_t>(vstreamer::stream_sender::k_wire_datagram_bytes);
    vstreamer::default_reactor_host sender_reactor;
    vstreamer::stream_sender sender;
    ASSERT_EQ(0, sender.configure("reactor", sender_reactor.name()));
    ASSERT_EQ(0, cfg(sender, "udp_transport", "127.0.0.1:9"));
    ASSERT_EQ(0, sender.open());
    EXPECT_EQ(k_dg - vstreamer::k_media_header_len, query_size_t(sender, "max_input"));
    /* One byte more is dropped and counted, not sent. */
    ASSERT_EQ(0, sender.input(vstreamer::test_pdu::make_stream_dgram(
                     0, k_dg - vstreamer::k_media_header_len + 1)));
    EXPECT_EQ(1U, query_size_t(sender, "oversized"));
    sender.close();
}

/* Every SDU is one media datagram, [0x80][u16 seq][SDU], and the sequence runs by one. */
TEST(StreamSenderTest, MediaDatagramsCarryOneRunningSequence)
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
    ASSERT_EQ(0, sender.open());

    uint8_t  wire[2048];
    size_t   wire_len = 0;
    uint16_t seq = 0;
    uint16_t seq0 = 0;
    for (uint16_t i = 0; i < 6; ++i)
    {
        ASSERT_EQ(0, sender.input(vstreamer::test_pdu::make_stream_dgram(i, 32)));
        ASSERT_TRUE(recv_one_udp(sniff, wire, sizeof(wire), &wire_len, 500));
        ASSERT_TRUE(vstreamer::media_header_parse(wire, wire_len, &seq));
        EXPECT_EQ(wire_len, vstreamer::k_media_header_len + 32U);
        if (0 == i)
        {
            seq0 = seq;
        }
        EXPECT_EQ(static_cast<uint16_t>(seq0 + i), seq);
    }

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
    EXPECT_EQ(-ENOENT, sender.open());
    ASSERT_EQ(0, cfg(sender, "reactor", ""));
    EXPECT_EQ(-EINVAL, sender.open());
    ASSERT_EQ(0, cfg(sender, "reactor", sender_reactor.name()));
    EXPECT_EQ(0, sender.open());
    sender.close();
}
