#include "components/stream_receiver.hpp"
#include "components/stream_sender.hpp"

#include "core/data_packet.hpp"
#include "core/packet_types.hpp"
#include "core/shared_sized_buffer.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
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

vstreamer::data_packet make_sock_packet(uint32_t counter, size_t payload_bytes)
{
    EXPECT_GE(payload_bytes, 4U);
    std::vector<uint8_t> storage(payload_bytes);
    storage[0] = static_cast<uint8_t>((counter >> 24) & 0xFF);
    storage[1] = static_cast<uint8_t>((counter >> 16) & 0xFF);
    storage[2] = static_cast<uint8_t>((counter >> 8) & 0xFF);
    storage[3] = static_cast<uint8_t>(counter & 0xFF);
    for (size_t i = 4; i < payload_bytes; i++)
    {
        storage[i] = static_cast<uint8_t>(i & 0xFF);
    }

    auto sd = std::make_unique<vstreamer::sock_data>();
    sd->pts = static_cast<int64_t>(counter);
    sd->buf = vstreamer::shared_sized_buffer::copy_from(storage.data(), storage.size());

    vstreamer::data_packet pkt;
    pkt.reset(std::move(sd));
    return pkt;
}

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
            vstreamer::data_packet out;
            const int              rc = receiver.output(0, out, timeout_ms);
            if (rc == -EAGAIN)
            {
                return;
            }
            ASSERT_EQ(0, rc);
            const auto &sd = vstreamer::data_packet::cast<vstreamer::sock_data>(out);
            ASSERT_GE(sd.buf.size(), 4U);
            const uint8_t *payload = sd.buf.u8();
            const size_t   payload_len = sd.buf.size();
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
        const vstreamer::data_packet in = make_sock_packet(static_cast<uint32_t>(i), k_payload);
        ASSERT_EQ(0, sender.input(0, in));
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
                EXPECT_EQ(-EINVAL, cfg_str(sender, "fec_n", "20"));
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

    const vstreamer::data_packet ok_pkt = make_sock_packet(0, 64);
    ASSERT_EQ(0, sender.input(0, ok_pkt));

    const vstreamer::data_packet big_pkt = make_sock_packet(1, limit + 1);
    ASSERT_EQ(0, sender.input(0, big_pkt));

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
            vstreamer::data_packet out;
            const int              rc = receiver.output(0, out, timeout_ms);
            if (rc == -EAGAIN)
            {
                return;
            }
            ASSERT_EQ(0, rc);
            const auto &sd = vstreamer::data_packet::cast<vstreamer::sock_data>(out);
            ASSERT_GE(sd.buf.size(), 4U);
            const uint8_t *payload = sd.buf.u8();
            const size_t   payload_len = sd.buf.size();
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
            const vstreamer::data_packet in =
                make_sock_packet(static_cast<uint32_t>(start_counter + i), 64);
            ASSERT_EQ(0, sender.input(0, in));
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
            vstreamer::data_packet out;
            const int              rc = receiver.output(0, out, timeout_ms);
            if (rc == -EAGAIN)
            {
                return;
            }
            ASSERT_EQ(0, rc);
            const auto &sd = vstreamer::data_packet::cast<vstreamer::sock_data>(out);
            ASSERT_GE(sd.buf.size(), 4U);
            const uint8_t *payload = sd.buf.u8();
            const size_t   payload_len = sd.buf.size();
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
            const vstreamer::data_packet in = make_sock_packet(static_cast<uint32_t>(10000 + i), 64);
            ASSERT_EQ(0, sender.input(0, in));
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
