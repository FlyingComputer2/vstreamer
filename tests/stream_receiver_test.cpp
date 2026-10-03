#include "components/stream_receiver.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
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
