#include "components/stream_receiver.hpp"
#include "components/stream_sender.hpp"

#include "core/shared_sized_buffer.hpp"
#include "core/stream_header.hpp"
#include "core/stream_telemetry.hpp"

#include <gtest/gtest.h>

#include "test_pdu_helpers.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

int cfg_str(vstreamer::component &c, std::string_view key, std::string_view value)
{
    std::string_view val = value;
    return c.configure(key, val);
}


/* Test-only NAT relay: forward media (drop every Nth), return link reports to the sender. */
class UdpNatRelay
{
public:
    ~UdpNatRelay()
    {
        stop();
    }

    void set_drop_burst(uint64_t after_media, int count)
    {
        drop_burst_after_ = after_media;
        drop_burst_count_ = count;
        drop_burst_remaining_ = count;
    }

    int start(int relay_port, int receiver_port, int drop_every_n)
    {
        drop_every_n_ = drop_every_n;
        drop_burst_after_ = 0;
        drop_burst_count_ = 0;
        drop_burst_remaining_ = 0;
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0)
        {
            return -errno;
        }
        sockaddr_in bind_addr {};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind_addr.sin_port = htons(static_cast<uint16_t>(relay_port));
        if (bind(fd_, reinterpret_cast<sockaddr *>(&bind_addr), sizeof(bind_addr)) < 0)
        {
            const int err = errno;
            close(fd_);
            fd_ = -1;
            return -err;
        }
        forward_.sin_family = AF_INET;
        forward_.sin_port = htons(static_cast<uint16_t>(receiver_port));
        forward_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        stop_ = false;
        thread_ = std::thread(&UdpNatRelay::loop, this);
        return 0;
    }

    void stop()
    {
        if (!thread_.joinable())
        {
            return;
        }
        stop_ = true;
        if (fd_ >= 0)
        {
            ::shutdown(fd_, SHUT_RDWR);
        }
        thread_.join();
        if (fd_ >= 0)
        {
            close(fd_);
            fd_ = -1;
        }
        stop_ = false;
    }

private:
    static bool is_link_report(const uint8_t *data, size_t len)
    {
        if (len != vstreamer::k_stream_header_len + vstreamer::k_stream_link_report_payload_len)
        {
            return false;
        }
        vstreamer::stream_header hdr {};
        const uint8_t *payload = nullptr;
        size_t         payload_len = 0;
        if (vstreamer::stream_header_parse(data, len, &hdr, &payload, &payload_len) < 0)
        {
            return false;
        }
        return !hdr.is_fec && !hdr.is_stream_data &&
               payload_len == vstreamer::k_stream_link_report_payload_len;
    }

    void loop()
    {
        uint8_t     buf[65536];
        sockaddr_in sender_addr {};
        socklen_t   sender_len = 0;
        bool        have_sender = false;
        uint64_t    media_seen = 0;

        while (!stop_.load(std::memory_order_relaxed))
        {
            pollfd pfd {fd_, POLLIN, 0};
            (void)poll(&pfd, 1, 50);
            if (stop_.load(std::memory_order_relaxed))
            {
                break;
            }
            for (;;)
            {
                sockaddr_in from {};
                socklen_t   from_len = sizeof(from);
                const ssize_t n = recvfrom(fd_, buf, sizeof(buf), MSG_DONTWAIT,
                                           reinterpret_cast<sockaddr *>(&from), &from_len);
                if (n <= 0)
                {
                    break;
                }
                if (is_link_report(buf, static_cast<size_t>(n)))
                {
                    if (have_sender)
                    {
                        (void)sendto(fd_, buf, static_cast<size_t>(n), 0,
                                     reinterpret_cast<sockaddr *>(&sender_addr), sender_len);
                    }
                    continue;
                }
                if (!have_sender)
                {
                    sender_addr = from;
                    sender_len = from_len;
                    have_sender = true;
                }
                ++media_seen;
                if (drop_burst_remaining_ > 0 && media_seen >= drop_burst_after_)
                {
                    --drop_burst_remaining_;
                    continue;
                }
                if (drop_every_n_ > 0 && (media_seen % static_cast<uint64_t>(drop_every_n_)) == 0)
                {
                    continue;
                }
                (void)sendto(fd_, buf, static_cast<size_t>(n), 0,
                             reinterpret_cast<sockaddr *>(&forward_), sizeof(forward_));
            }
        }
    }

    int                fd_ = -1;
    int                drop_every_n_ = 0;
    uint64_t           drop_burst_after_ = 0;
    int                drop_burst_count_ = 0;
    int                drop_burst_remaining_ = 0;
    sockaddr_in        forward_ {};
    std::atomic<bool>  stop_ {false};
    std::thread        thread_;
};

uint32_t read_counter_be32(const uint8_t *data, size_t len)
{
    if (len < 4)
    {
        return UINT32_MAX;
    }
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

void run_link(int n_packets,
              const std::function<void(vstreamer::stream_sender &,
                                       vstreamer::stream_receiver &)> &setup_fn,
              const std::function<void(vstreamer::stream_sender &, int packets_sent)> &mid_fn,
              const std::function<void(vstreamer::stream_sender &,
                                       vstreamer::stream_receiver &)> &post_fn = nullptr)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    vstreamer::stream_sender   sender;
    vstreamer::stream_receiver receiver;

    const std::string host_port = "127.0.0.1:" + std::to_string(port);
    ASSERT_EQ(0, cfg_str(sender, "stream", host_port));
    ASSERT_EQ(0, cfg_str(receiver, "listen", host_port));

    if (setup_fn)
    {
        setup_fn(sender, receiver);
    }

    ASSERT_EQ(0, receiver.open());
    ASSERT_EQ(0, sender.open());
    ASSERT_EQ(0, sender.set_enabled(true, 0));

    constexpr size_t k_payload = 64;
    uint32_t         expect_next = 0;

    auto drain_outputs = [&](int timeout_ms) {
        for (;;)
        {
            vstreamer::component_pdu out;
            const int              rc = receiver.output(out);
            if (rc == -EAGAIN)
            {
                return;
            }
            ASSERT_EQ(0, rc);
            ASSERT_GE(out.sdu.size(), 4U);
            const uint8_t *payload = out.sdu.u8();
            const size_t   payload_len = out.sdu.size();
            const uint32_t ctr = read_counter_be32(payload, payload_len);
            ASSERT_EQ(ctr, expect_next);
            expect_next++;
        }
    };

    for (int i = 0; i < n_packets; i++)
    {
        if (mid_fn)
        {
            mid_fn(sender, i);
        }
        vstreamer::component_pdu in = vstreamer::test_pdu::make_stream_dgram(static_cast<uint32_t>(i), k_payload);
        ASSERT_EQ(0, sender.input(std::move(in)));
        drain_outputs(0);
        /* Avoid overrunning the UDP recv path on loopback (drops look like out-of-order FEC). */
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (expect_next < static_cast<uint32_t>(n_packets))
    {
        ASSERT_LT(std::chrono::steady_clock::now(), drain_deadline)
            << "stuck waiting for counter " << expect_next;
        drain_outputs(200);
    }

    if (post_fn)
    {
        post_fn(sender, receiver);
    }

    sender.close();
    receiver.close();
}

}  // namespace

TEST(LoopbackTest, FecK6N8InOrder)
{
    run_link(
        2000,
        [](vstreamer::stream_sender &sender, vstreamer::stream_receiver &) {
            ASSERT_EQ(0, cfg_str(sender, "fec", "block"));
            ASSERT_EQ(0, cfg_str(sender, "fec_k", "6"));
            ASSERT_EQ(0, cfg_str(sender, "fec_n", "8"));
            ASSERT_EQ(0, cfg_str(sender, "max_kbps", "0"));
        },
        nullptr);
}

TEST(LoopbackTest, RuntimeFecNRejectedThenAccepted)
{
    run_link(
        2000,
        [](vstreamer::stream_sender &sender, vstreamer::stream_receiver &) {
            ASSERT_EQ(0, cfg_str(sender, "fec", "block"));
            ASSERT_EQ(0, cfg_str(sender, "fec_k", "6"));
            ASSERT_EQ(0, cfg_str(sender, "fec_n", "8"));
            ASSERT_EQ(0, cfg_str(sender, "max_kbps", "0"));
        },
        [](vstreamer::stream_sender &sender, int packets_sent) {
            if (packets_sent == 500)
            {
                EXPECT_EQ(-EINVAL, cfg_str(sender, "fec_n", "32"));
                EXPECT_EQ(0, cfg_str(sender, "fec_n", "10"));
            }
        });
}

TEST(LoopbackTest, MaxDatagram600InOrder)
{
    run_link(
        2000,
        [](vstreamer::stream_sender &sender, vstreamer::stream_receiver &receiver) {
            ASSERT_EQ(0, cfg_str(sender, "max_datagram", "600"));
            ASSERT_EQ(0, cfg_str(receiver, "max_datagram", "600"));
            ASSERT_EQ(0, cfg_str(sender, "fec", "block"));
            ASSERT_EQ(0, cfg_str(sender, "fec_k", "6"));
            ASSERT_EQ(0, cfg_str(sender, "fec_n", "8"));
            ASSERT_EQ(0, cfg_str(sender, "max_kbps", "0"));
        },
        nullptr,
        [](vstreamer::stream_sender &sender, vstreamer::stream_receiver &) {
            std::string overs;
            ASSERT_EQ(0, sender.query("fec_oversized", &overs));
            EXPECT_EQ("0", overs);
        });
}

TEST(LoopbackTest, MaxInputOversizedNotSent)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    vstreamer::stream_sender   sender;
    vstreamer::stream_receiver receiver;
    const std::string          host_port = "127.0.0.1:" + std::to_string(port);
    ASSERT_EQ(0, cfg_str(sender, "stream", host_port));
    ASSERT_EQ(0, cfg_str(receiver, "listen", host_port));
    ASSERT_EQ(0, cfg_str(sender, "max_datagram", "600"));
    ASSERT_EQ(0, cfg_str(receiver, "max_datagram", "600"));
    ASSERT_EQ(0, cfg_str(sender, "fec", "block"));
    ASSERT_EQ(0, cfg_str(sender, "fec_k", "6"));
    ASSERT_EQ(0, cfg_str(sender, "fec_n", "8"));
    ASSERT_EQ(0, receiver.open());
    ASSERT_EQ(0, sender.open());
    ASSERT_EQ(0, sender.set_enabled(true, 0));

    std::string max_in;
    ASSERT_EQ(0, sender.query("max_input", &max_in));
    const size_t limit = static_cast<size_t>(std::strtoul(max_in.c_str(), nullptr, 10));
    ASSERT_GT(limit, 64U);

    vstreamer::component_pdu ok_pkt = vstreamer::test_pdu::make_stream_dgram(0, 64);
    ASSERT_EQ(0, sender.input(std::move(ok_pkt)));

    vstreamer::component_pdu big_pkt = vstreamer::test_pdu::make_stream_dgram(1, limit + 1);
    ASSERT_EQ(0, sender.input(std::move(big_pkt)));

    std::string overs;
    ASSERT_EQ(0, sender.query("fec_oversized", &overs));
    EXPECT_NE("0", overs);

    sender.close();
    receiver.close();
}

TEST(LoopbackTest, FecNoneFromStart)
{
    run_link(
        2000,
        [](vstreamer::stream_sender &sender, vstreamer::stream_receiver &) {
            ASSERT_EQ(0, cfg_str(sender, "fec", "none"));
            ASSERT_EQ(0, cfg_str(sender, "max_kbps", "0"));
        },
        nullptr);
}

TEST(LoopbackTest, FecModeSwitchMidStream)
{
    run_link(
        2000,
        [](vstreamer::stream_sender &sender, vstreamer::stream_receiver &) {
            ASSERT_EQ(0, cfg_str(sender, "fec", "block"));
            ASSERT_EQ(0, cfg_str(sender, "fec_k", "6"));
            ASSERT_EQ(0, cfg_str(sender, "fec_n", "8"));
            ASSERT_EQ(0, cfg_str(sender, "max_kbps", "0"));
        },
        [](vstreamer::stream_sender &sender, int packets_sent) {
            if (packets_sent == 600)
            {
                EXPECT_EQ(0, cfg_str(sender, "fec", "none"));
            }
            if (packets_sent == 1200)
            {
                EXPECT_EQ(0, cfg_str(sender, "fec", "block"));
            }
        });
}

TEST(LoopbackTest, SenderRestartMidStream)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    vstreamer::stream_receiver receiver;
    const std::string          host_port = "127.0.0.1:" + std::to_string(port);
    ASSERT_EQ(0, cfg_str(receiver, "listen", host_port));
    ASSERT_EQ(0, receiver.open());

    uint32_t expect_next = 0;
    auto     drain_outputs = [&](int timeout_ms) {
        for (;;)
        {
            vstreamer::component_pdu out;
            const int              rc = receiver.output(out);
            if (rc == -EAGAIN)
            {
                return;
            }
            ASSERT_EQ(0, rc);
            ASSERT_GE(out.sdu.size(), 4U);
            const uint8_t *payload = out.sdu.u8();
            const size_t   payload_len = out.sdu.size();
            const uint32_t ctr = read_counter_be32(payload, payload_len);
            ASSERT_EQ(ctr, expect_next);
            expect_next++;
        }
    };

    auto run_sender_packets = [&](int n_packets, int start_counter) {
        vstreamer::stream_sender sender;
        ASSERT_EQ(0, cfg_str(sender, "stream", host_port));
        ASSERT_EQ(0, cfg_str(sender, "fec", "block"));
        ASSERT_EQ(0, cfg_str(sender, "fec_k", "6"));
        ASSERT_EQ(0, cfg_str(sender, "fec_n", "8"));
        ASSERT_EQ(0, cfg_str(sender, "max_kbps", "0"));
        ASSERT_EQ(0, sender.open());
        ASSERT_EQ(0, sender.set_enabled(true, 0));
        const uint32_t end_counter = static_cast<uint32_t>(start_counter + n_packets);
        for (int i = 0; i < n_packets; i++)
        {
            vstreamer::component_pdu in =
                vstreamer::test_pdu::make_stream_dgram(static_cast<uint32_t>(start_counter + i), 64);
            ASSERT_EQ(0, sender.input(std::move(in)));
            drain_outputs(0);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (expect_next < end_counter)
        {
            ASSERT_LT(std::chrono::steady_clock::now(), drain_deadline)
                << "stuck waiting for counter " << expect_next;
            drain_outputs(200);
        }
        sender.close();
    };

    run_sender_packets(400, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    expect_next = 10000;
    std::chrono::steady_clock::time_point t_restart;
    bool                                  saw_resume = false;
    auto       drain_outputs_resume = [&](int timeout_ms) {
        for (;;)
        {
            vstreamer::component_pdu out;
            const int              rc = receiver.output(out);
            if (rc == -EAGAIN)
            {
                return;
            }
            ASSERT_EQ(0, rc);
            ASSERT_GE(out.sdu.size(), 4U);
            const uint8_t *payload = out.sdu.u8();
            const size_t   payload_len = out.sdu.size();
            const uint32_t ctr = read_counter_be32(payload, payload_len);
            ASSERT_EQ(ctr, expect_next);
            if (!saw_resume && expect_next == 10000u)
            {
                saw_resume = true;
                ASSERT_LT(std::chrono::steady_clock::now() - t_restart,
                          std::chrono::milliseconds(300));
            }
            expect_next++;
        }
    };

    {
        vstreamer::stream_sender sender;
        ASSERT_EQ(0, cfg_str(sender, "stream", host_port));
        ASSERT_EQ(0, cfg_str(sender, "fec", "block"));
        ASSERT_EQ(0, cfg_str(sender, "fec_k", "6"));
        ASSERT_EQ(0, cfg_str(sender, "fec_n", "8"));
        ASSERT_EQ(0, cfg_str(sender, "max_kbps", "0"));
        ASSERT_EQ(0, sender.open());
        ASSERT_EQ(0, sender.set_enabled(true, 0));
        t_restart = std::chrono::steady_clock::now();
        for (int i = 0; i < 400; i++)
        {
            vstreamer::component_pdu in = vstreamer::test_pdu::make_stream_dgram(static_cast<uint32_t>(10000 + i), 64);
            ASSERT_EQ(0, sender.input(std::move(in)));
            drain_outputs_resume(0);
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (expect_next < 10400u)
        {
            ASSERT_LT(std::chrono::steady_clock::now(), drain_deadline)
                << "stuck waiting for counter " << expect_next;
            drain_outputs_resume(200);
        }
        sender.close();
    }
    ASSERT_TRUE(saw_resume);
    ASSERT_EQ(expect_next, 10400u);
    receiver.close();
}

TEST(LoopbackTest, ReverseTelemetryCountersMatch)
{
    const int rcv_port = ephemeral_udp_port();
    const int relay_port = ephemeral_udp_port();
    ASSERT_GT(rcv_port, 0);
    ASSERT_GT(relay_port, 0);

    UdpNatRelay relay;
    ASSERT_EQ(0, relay.start(relay_port, rcv_port, 10));

    vstreamer::stream_sender   sender;
    vstreamer::stream_receiver receiver;

    const std::string relay_host = "127.0.0.1:" + std::to_string(relay_port);
    const std::string rcv_host = "127.0.0.1:" + std::to_string(rcv_port);
    ASSERT_EQ(0, cfg_str(sender, "stream", relay_host));
    ASSERT_EQ(0, cfg_str(receiver, "listen", rcv_host));
    ASSERT_EQ(0, cfg_str(receiver, "telemetry_ms", "20"));
    ASSERT_EQ(0, cfg_str(sender, "fec", "block"));
    ASSERT_EQ(0, cfg_str(sender, "fec_k", "6"));
    ASSERT_EQ(0, cfg_str(sender, "fec_n", "8"));
    ASSERT_EQ(0, cfg_str(sender, "max_kbps", "0"));

    ASSERT_EQ(0, receiver.open());
    ASSERT_EQ(0, sender.open());
    ASSERT_EQ(0, sender.set_enabled(true, 0));

    constexpr size_t k_payload = 64;
    constexpr int    k_packets = 200;
    for (int i = 0; i < k_packets; ++i)
    {
        vstreamer::component_pdu in = vstreamer::test_pdu::make_stream_dgram(static_cast<uint32_t>(i), k_payload);
        ASSERT_EQ(0, sender.input(std::move(in)));
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const auto rcv_cnt = receiver.link_counters_snapshot();
    const auto peer = sender.peer_link_snapshot();
    ASSERT_TRUE(peer.have);
    EXPECT_EQ(rcv_cnt.udp_packet_received, peer.report.counters.udp_packet_received);
    EXPECT_EQ(rcv_cnt.fec_packet_received, peer.report.counters.fec_packet_received);
    EXPECT_EQ(rcv_cnt.udp_gap_count, peer.report.counters.udp_gap_count);
    EXPECT_EQ(rcv_cnt.fec_gap_count, peer.report.counters.fec_gap_count);
    EXPECT_GT(rcv_cnt.udp_gap_count, 0U);
    EXPECT_EQ(0U, peer.reports_lost);

    sender.close();
    receiver.close();
    relay.stop();
}

TEST(LoopbackTest, FecKn31WithDropEveryNRecovers)
{
    const int rcv_port = ephemeral_udp_port();
    const int relay_port = ephemeral_udp_port();
    ASSERT_GT(rcv_port, 0);
    ASSERT_GT(relay_port, 0);

    UdpNatRelay relay;
    ASSERT_EQ(0, relay.start(relay_port, rcv_port, 100));

    vstreamer::stream_sender   sender;
    vstreamer::stream_receiver receiver;
    const std::string          relay_host = "127.0.0.1:" + std::to_string(relay_port);
    const std::string          rcv_host = "127.0.0.1:" + std::to_string(rcv_port);
    ASSERT_EQ(0, cfg_str(sender, "stream", relay_host));
    ASSERT_EQ(0, cfg_str(receiver, "listen", rcv_host));
    ASSERT_EQ(0, cfg_str(sender, "fec", "block"));
    ASSERT_EQ(0, cfg_str(sender, "fec_n", "31"));
    ASSERT_EQ(0, cfg_str(sender, "fec_k", "31"));
    ASSERT_EQ(0, cfg_str(sender, "max_kbps", "0"));

    ASSERT_EQ(0, receiver.open());
    ASSERT_EQ(0, sender.open());
    ASSERT_EQ(0, sender.set_enabled(true, 0));

    constexpr int k_packets = 80;
    for (int i = 0; i < k_packets; ++i)
    {
        ASSERT_EQ(0, sender.input(vstreamer::test_pdu::make_stream_dgram(static_cast<uint32_t>(i), 64)));
        std::this_thread::sleep_for(std::chrono::microseconds(800));
    }
    uint32_t delivered = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (delivered < static_cast<uint32_t>(k_packets) &&
           std::chrono::steady_clock::now() < deadline)
    {
        vstreamer::component_pdu out;
        if (receiver.output(out) == 0)
        {
            ++delivered;
        }
    }
    EXPECT_EQ(static_cast<uint32_t>(k_packets), delivered);
    const auto cnt = receiver.link_counters_snapshot();
    EXPECT_EQ(0U, cnt.fec_gap_count);

    sender.close();
    receiver.close();
    relay.stop();
}

TEST(LoopbackTest, RawModeFecGapMatchesUdpGap)
{
    const int rcv_port = ephemeral_udp_port();
    const int relay_port = ephemeral_udp_port();
    ASSERT_GT(rcv_port, 0);
    ASSERT_GT(relay_port, 0);

    UdpNatRelay relay;
    ASSERT_EQ(0, relay.start(relay_port, rcv_port, 5));

    vstreamer::stream_sender   sender;
    vstreamer::stream_receiver receiver;
    const std::string          relay_host = "127.0.0.1:" + std::to_string(relay_port);
    const std::string          rcv_host = "127.0.0.1:" + std::to_string(rcv_port);
    ASSERT_EQ(0, cfg_str(sender, "stream", relay_host));
    ASSERT_EQ(0, cfg_str(receiver, "listen", rcv_host));
    ASSERT_EQ(0, cfg_str(sender, "fec", "none"));
    ASSERT_EQ(0, cfg_str(sender, "max_kbps", "0"));

    ASSERT_EQ(0, receiver.open());
    ASSERT_EQ(0, sender.open());
    ASSERT_EQ(0, sender.set_enabled(true, 0));

    for (int i = 0; i < 200; ++i)
    {
        ASSERT_EQ(0, sender.input(vstreamer::test_pdu::make_stream_dgram(static_cast<uint32_t>(i), 64)));
        std::this_thread::sleep_for(std::chrono::microseconds(300));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto cnt = receiver.link_counters_snapshot();
    EXPECT_EQ(cnt.udp_gap_count, cnt.fec_gap_count);

    sender.close();
    receiver.close();
    relay.stop();
}

TEST(LoopbackTest, PeerReportIntervalNearTelemetryMs)
{
    const int rcv_port = ephemeral_udp_port();
    const int relay_port = ephemeral_udp_port();
    ASSERT_GT(rcv_port, 0);
    ASSERT_GT(relay_port, 0);

    UdpNatRelay relay;
    ASSERT_EQ(0, relay.start(relay_port, rcv_port, 0));

    vstreamer::stream_sender   sender;
    vstreamer::stream_receiver receiver;
    const std::string          relay_host = "127.0.0.1:" + std::to_string(relay_port);
    const std::string          rcv_host = "127.0.0.1:" + std::to_string(rcv_port);
    ASSERT_EQ(0, cfg_str(sender, "stream", relay_host));
    ASSERT_EQ(0, cfg_str(receiver, "listen", rcv_host));
    ASSERT_EQ(0, cfg_str(receiver, "telemetry_ms", "100"));
    ASSERT_EQ(0, cfg_str(sender, "fec", "none"));

    ASSERT_EQ(0, receiver.open());
    ASSERT_EQ(0, sender.open());
    ASSERT_EQ(0, sender.set_enabled(true, 0));

    for (int i = 0; i < 30; ++i)
    {
        ASSERT_EQ(0, sender.input(vstreamer::test_pdu::make_stream_dgram(static_cast<uint32_t>(i), 64)));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(450));

    std::string interval_s;
    ASSERT_EQ(0, sender.query("peer_report_interval_ms", &interval_s));
    const int64_t observed = std::strtoll(interval_s.c_str(), nullptr, 10);
    EXPECT_GE(observed, 50);
    EXPECT_LE(observed, 150);

    sender.close();
    receiver.close();
    relay.stop();
}

TEST(LoopbackTest, RawMaxInputPayloadDelivered)
{
    const int port = ephemeral_udp_port();
    ASSERT_GT(port, 0);

    vstreamer::stream_sender   sender;
    vstreamer::stream_receiver receiver;
    const std::string          host_port = "127.0.0.1:" + std::to_string(port);
    ASSERT_EQ(0, cfg_str(sender, "stream", host_port));
    ASSERT_EQ(0, cfg_str(receiver, "listen", host_port));
    ASSERT_EQ(0, cfg_str(sender, "max_datagram", "1476"));
    ASSERT_EQ(0, cfg_str(receiver, "max_datagram", "1476"));
    ASSERT_EQ(0, cfg_str(sender, "fec", "none"));
    ASSERT_EQ(0, cfg_str(sender, "max_kbps", "0"));

    ASSERT_EQ(0, receiver.open());
    ASSERT_EQ(0, sender.open());
    ASSERT_EQ(0, sender.set_enabled(true, 0));

    std::string max_in_s;
    ASSERT_EQ(0, sender.query("max_input", &max_in_s));
    const size_t max_in = static_cast<size_t>(std::strtoull(max_in_s.c_str(), nullptr, 10));
    EXPECT_EQ(1472U, max_in);

    auto send_and_expect = [&](size_t payload_len, uint32_t tag) {
        std::vector<uint8_t> storage(payload_len);
        storage[0] = static_cast<uint8_t>((tag >> 24) & 0xFF);
        storage[1] = static_cast<uint8_t>((tag >> 16) & 0xFF);
        storage[2] = static_cast<uint8_t>((tag >> 8) & 0xFF);
        storage[3] = static_cast<uint8_t>(tag & 0xFF);
        for (size_t i = 4; i < payload_len; ++i)
        {
            storage[i] = static_cast<uint8_t>(i & 0xFF);
        }
        ASSERT_EQ(0, sender.input(std::move(vstreamer::test_pdu::make_stream_dgram(tag, payload_len))));
        vstreamer::component_pdu out;
        const auto             deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline)
        {
            const int rc = receiver.output(out);
            if (0 == rc)
            {
                ASSERT_EQ(payload_len, out.sdu.size());
                EXPECT_EQ(0, std::memcmp(storage.data(), out.sdu.u8(), payload_len));
                return;
            }
            ASSERT_TRUE(rc == -EAGAIN);
        }
        FAIL() << "no output for payload len " << payload_len;
    };

    send_and_expect(max_in, 1U);
    send_and_expect(1466U, 2U);

    std::string stats;
    ASSERT_EQ(0, receiver.query("stats", &stats));
    EXPECT_NE(std::string::npos, stats.find("dropped=0"));

    sender.close();
    receiver.close();
}

TEST(LoopbackTest, FecBlockNoneBlockNoExtraGap)
{
    run_link(
        400,
        [](vstreamer::stream_sender &sender, vstreamer::stream_receiver &) {
            ASSERT_EQ(0, cfg_str(sender, "fec", "block"));
            ASSERT_EQ(0, cfg_str(sender, "fec_k", "6"));
            ASSERT_EQ(0, cfg_str(sender, "fec_n", "8"));
            ASSERT_EQ(0, cfg_str(sender, "max_kbps", "0"));
        },
        [](vstreamer::stream_sender &sender, int packets_sent) {
            if (100 == packets_sent)
            {
                EXPECT_EQ(0, cfg_str(sender, "fec", "none"));
            }
            if (200 == packets_sent)
            {
                EXPECT_EQ(0, cfg_str(sender, "fec", "block"));
            }
        },
        [](vstreamer::stream_sender &, vstreamer::stream_receiver &receiver) {
            const auto cnt = receiver.link_counters_snapshot();
            EXPECT_EQ(0U, cnt.fec_gap_count);
        });
}

TEST(LoopbackTest, WholeBlockLossViaBurstDrop)
{
    const int rcv_port = ephemeral_udp_port();
    const int relay_port = ephemeral_udp_port();
    ASSERT_GT(rcv_port, 0);
    ASSERT_GT(relay_port, 0);

    UdpNatRelay relay;
    ASSERT_EQ(0, relay.start(relay_port, rcv_port, 0));
    /* k=6 n=8 → 8 air shards per block; drop the second block wholesale. */
    relay.set_drop_burst(8, 8);

    vstreamer::stream_sender   sender;
    vstreamer::stream_receiver receiver;
    const std::string          relay_host = "127.0.0.1:" + std::to_string(relay_port);
    const std::string          rcv_host = "127.0.0.1:" + std::to_string(rcv_port);
    ASSERT_EQ(0, cfg_str(sender, "stream", relay_host));
    ASSERT_EQ(0, cfg_str(receiver, "listen", rcv_host));
    ASSERT_EQ(0, cfg_str(sender, "fec", "block"));
    ASSERT_EQ(0, cfg_str(sender, "fec_k", "6"));
    ASSERT_EQ(0, cfg_str(sender, "fec_n", "8"));
    ASSERT_EQ(0, cfg_str(sender, "max_kbps", "0"));

    ASSERT_EQ(0, receiver.open());
    ASSERT_EQ(0, sender.open());
    ASSERT_EQ(0, sender.set_enabled(true, 0));

    for (int i = 0; i < 24; ++i)
    {
        ASSERT_EQ(0, sender.input(vstreamer::test_pdu::make_stream_dgram(static_cast<uint32_t>(i), 64)));
        std::this_thread::sleep_for(std::chrono::microseconds(400));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    const auto cnt = receiver.link_counters_snapshot();
    EXPECT_EQ(6U, cnt.fec_gap_count);

    sender.close();
    receiver.close();
    relay.stop();
}
