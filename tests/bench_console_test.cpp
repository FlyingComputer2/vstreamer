#include "test_app/stream_sdl/bench_console.hpp"
#include "test_app/stream_sdl/link_emulator.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>

namespace
{

std::string udp_console_exchange(int console_port, const char *line, int timeout_ms = 2000)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return {};
    }

    sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(console_port));
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const size_t len = std::strlen(line);
    if (sendto(fd, line, len, 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst)) !=
        static_cast<ssize_t>(len))
    {
        close(fd);
        return {};
    }

    char buf[256];
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        pollfd pfd {fd, POLLIN, 0};
        if (poll(&pfd, 1, 50) <= 0)
        {
            continue;
        }
        const ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n > 0)
        {
            buf[n] = '\0';
            close(fd);
            return std::string(buf);
        }
    }
    close(fd);
    return {};
}

}  // namespace

TEST(BenchConsoleTest, SetConstantLossOk)
{
    vstreamer::test_app::link_emulator link;
    vstreamer::test_app::bench_console console;

    const int port = 19200;
    ASSERT_EQ(0, console.start(link, port));

    const std::string reply = udp_console_exchange(port, "set_constant_loss 5\n");
    console.stop();

    EXPECT_EQ("ok\n", reply);
    EXPECT_DOUBLE_EQ(5., link.constant_loss());
}
