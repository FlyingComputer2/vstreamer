#include "apps/stream_sdl_test/link_emulator.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <thread>
#include <vector>

namespace
{

int bind_udp_loopback(int port)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return -1;
    }
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

int recv_until(int fd, int expected, int timeout_ms)
{
    int got = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    uint8_t buf[2048];
    while (got < expected && std::chrono::steady_clock::now() < deadline)
    {
        pollfd pfd {fd, POLLIN, 0};
        const int pr = poll(&pfd, 1, 50);
        if (pr <= 0)
        {
            continue;
        }
        const ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n > 0)
        {
            ++got;
        }
    }
    return got;
}

}  // namespace

TEST(LinkEmulatorTest, BurstNoImpairmentDeliversAll)
{
    constexpr int k_ingress = 19100;
    constexpr int k_egress = 19101;
    const int     recv_fd = bind_udp_loopback(k_egress);
    ASSERT_GE(recv_fd, 0);

    vstreamer::test_app::link_emulator ch;
    ch.set_queue_depth(32);
    ch.set_max_kbps(0.);
    ch.set_constant_loss(0.);
    ASSERT_EQ(0, ch.start(k_ingress, "127.0.0.1", k_egress, 0, nullptr, 0));

    const int send_fd = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(send_fd, 0);
    sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons(static_cast<uint16_t>(k_ingress));

    constexpr int k_pkts = 300;
    uint8_t       payload[8] = {0xAB};
    for (int i = 0; i < k_pkts; ++i)
    {
        payload[0] = static_cast<uint8_t>(i & 0xFF);
        ASSERT_EQ(static_cast<ssize_t>(sizeof(payload)),
                  sendto(send_fd, payload, sizeof(payload), 0,
                         reinterpret_cast<sockaddr *>(&dst), sizeof(dst)));
    }

    const int got = recv_until(recv_fd, k_pkts, 3000);
    const auto stats = ch.forward_stats_snapshot();

    close(send_fd);
    close(recv_fd);
    ch.stop();

    EXPECT_EQ(k_pkts, got);
    EXPECT_EQ(static_cast<uint64_t>(k_pkts), stats.pkts_out);
    EXPECT_EQ(0U, stats.dropped_queue);
}

TEST(LinkEmulatorTest, RateLimitAccounting)
{
    constexpr int k_ingress = 19110;
    constexpr int k_egress = 19111;
    const int     recv_fd = bind_udp_loopback(k_egress);
    ASSERT_GE(recv_fd, 0);

    vstreamer::test_app::link_emulator ch;
    ch.set_queue_depth(0);
    ch.set_max_kbps(50.);
    ch.set_drop_dt_ms(10);
    ch.set_constant_loss(0.);
    ASSERT_EQ(0, ch.start(k_ingress, "127.0.0.1", k_egress, 0, nullptr, 0));

    const int send_fd = socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(send_fd, 0);
    sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons(static_cast<uint16_t>(k_ingress));

    constexpr int k_pkts = 200;
    uint8_t       payload[64] {};
    for (int i = 0; i < k_pkts; ++i)
    {
        ASSERT_EQ(static_cast<ssize_t>(sizeof(payload)),
                  sendto(send_fd, payload, sizeof(payload), 0,
                         reinterpret_cast<sockaddr *>(&dst), sizeof(dst)));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const auto stats = ch.forward_stats_snapshot();
    close(send_fd);
    close(recv_fd);
    ch.stop();

    EXPECT_GT(stats.dropped_rate, 0U);
    EXPECT_EQ(stats.pkts_in,
              stats.pkts_out + stats.dropped_queue + stats.dropped_rate + stats.dropped_loss);
}
