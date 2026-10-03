#include "components/stream_receiver.hpp"
#include "components/stream_sender.hpp"

#include "core/component.hpp"
#include "core/data_packet.hpp"
#include "core/shared_sized_buffer.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
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
