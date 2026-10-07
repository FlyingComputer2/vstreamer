#include "apps/common/app_console.hpp"

#include <gtest/gtest.h>


#include <arpa/inet.h>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#include <string>

namespace
{

std::string udp_exchange(int port, const char *line)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return {};
    }
    sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(port));
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const size_t len = std::strlen(line);
    sendto(fd, line, len, 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
    char buf[256];
    const ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, nullptr, nullptr);
    close(fd);
    if (n <= 0)
    {
        return {};
    }
    buf[n] = '\0';
    return std::string(buf);
}

}  // namespace

TEST(AppConsoleTest, PingReply)
{
    vstreamer::apps::app_console console;
    const int                    port = 19301;
    ASSERT_EQ(0, console.start(port));
    EXPECT_EQ("pong\n", udp_exchange(port, "ping\n"));
    console.stop();
}

TEST(AppConsoleTest, RegisteredHandlerWins)
{
    vstreamer::apps::app_console console;
    console.add_handler(
        [](const char *line, std::string &reply) {
            if (0 == std::strcmp(line, "custom"))
            {
                reply = "handled\n";
                return true;
            }
            return false;
        },
        "custom\n");
    std::string reply;
    EXPECT_TRUE(console.handle_line("custom", reply));
    EXPECT_EQ("handled\n", reply);
}

TEST(AppConsoleTest, UnknownCommand)
{
    vstreamer::apps::app_console console;
    std::string                  reply;
    EXPECT_TRUE(console.handle_line("not_a_verb", reply));
    EXPECT_EQ("err: unknown command\n", reply);
}

TEST(AppConsoleTest, HelpListsHandlerText)
{
    vstreamer::apps::app_console console;
    console.add_handler(
        [](const char *, std::string &) { return false; }, "my_verb <arg>\n");
    std::string reply;
    EXPECT_TRUE(console.handle_line("help", reply));
    EXPECT_NE(reply.find("my_verb <arg>"), std::string::npos);
}
