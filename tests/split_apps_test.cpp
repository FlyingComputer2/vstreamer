#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern char **environ;

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

std::string udp_exchange(int port, const char *line)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return {};
    }
    timeval tv {};
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(port));
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const size_t len = std::strlen(line);
    (void)sendto(fd, line, len, 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
    char buf[512];
    const ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, nullptr, nullptr);
    close(fd);
    if (n <= 0)
    {
        return {};
    }
    buf[n] = '\0';
    return std::string(buf);
}

int64_t metric_from_console(int port, const char *metric_name)
{
    (void)udp_exchange(port, "metrics\n");
    std::string cmd = std::string("get_metric ") + metric_name + "\n";
    const std::string reply = udp_exchange(port, cmd.c_str());
    if (reply.empty() || reply.rfind("err", 0) == 0)
    {
        return -1;
    }
    const char *num = reply.c_str();
    const char *eq = std::strchr(num, '=');
    if (nullptr != eq)
    {
        num = eq + 1;
    }
    char *end = nullptr;
    const long v = std::strtol(num, &end, 10);
    if (end == num)
    {
        return -1;
    }
    return static_cast<int64_t>(v);
}

pid_t spawn_process(const char *bin, char *const argv[], const char *const envp[])
{
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, bin, nullptr, nullptr, argv, const_cast<char **>(envp));
    if (rc != 0)
    {
        return -1;
    }
    return pid;
}

bool wait_exit(pid_t pid, int seconds)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline)
    {
        int   status = 0;
        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid)
        {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
        if (w < 0)
        {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

void stop_process(pid_t pid)
{
    if (pid <= 0)
    {
        return;
    }
    kill(pid, SIGINT);
    if (!wait_exit(pid, 2))
    {
        kill(pid, SIGTERM);
        (void)wait_exit(pid, 2);
        kill(pid, SIGKILL);
        (void)waitpid(pid, nullptr, 0);
    }
}

}  // namespace

#ifndef SDL_STREAM_RECEIVER_BIN
#define SDL_STREAM_RECEIVER_BIN ""
#endif
#ifndef UVC_STREAM_SENDER_BIN
#define UVC_STREAM_SENDER_BIN ""
#endif

TEST(SplitAppsTest, SenderReceiverLinkAndCleanShutdown)
{
    if (SDL_STREAM_RECEIVER_BIN[0] == '\0' || UVC_STREAM_SENDER_BIN[0] == '\0')
    {
        GTEST_SKIP() << "split app binaries not built";
    }

    const int media_port = ephemeral_udp_port();
    const int rx_console = ephemeral_udp_port();
    const int tx_console = ephemeral_udp_port();
    ASSERT_GT(media_port, 0);
    ASSERT_GT(rx_console, 0);
    ASSERT_GT(tx_console, 0);

    char listen[64];
    char rx_cons[64];
    char peer[64];
    char tx_cons[64];
    std::snprintf(listen, sizeof(listen), "127.0.0.1:%d", media_port);
    std::snprintf(rx_cons, sizeof(rx_cons), "127.0.0.1:%d", rx_console);
    std::snprintf(peer, sizeof(peer), "127.0.0.1:%d", media_port);
    std::snprintf(tx_cons, sizeof(tx_cons), "127.0.0.1:%d", tx_console);

    char *rcv_argv[] = {const_cast<char *>(SDL_STREAM_RECEIVER_BIN),
                        const_cast<char *>("--listen"),
                        listen,
                        const_cast<char *>("--console"),
                        rx_cons,
                        nullptr};
    std::vector<std::string> env_storage;
    for (char **e = environ; nullptr != *e; ++e)
    {
        env_storage.emplace_back(*e);
    }
    env_storage.emplace_back("VSTREAMER_SKIP_DECODE=1");
    std::vector<char *> envp;
    for (std::string &s : env_storage)
    {
        envp.push_back(s.data());
    }
    envp.push_back(nullptr);
    const pid_t rcv_pid = spawn_process(SDL_STREAM_RECEIVER_BIN, rcv_argv, envp.data());
    ASSERT_GT(rcv_pid, 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    char *snd_argv[] = {const_cast<char *>(UVC_STREAM_SENDER_BIN),
                        const_cast<char *>("--peer"),
                        peer,
                        const_cast<char *>("--device"),
                        const_cast<char *>("/nonexistent"),
                        const_cast<char *>("--console"),
                        tx_cons,
                        nullptr};
    const pid_t snd_pid = spawn_process(UVC_STREAM_SENDER_BIN, snd_argv, nullptr);
    ASSERT_GT(snd_pid, 0);

    bool ok = false;
    for (int i = 0; i < 50; ++i)
    {
        const int64_t rx_pkts = metric_from_console(rx_console, "stream_receiver.in_packets");
        const int64_t peer_fec =
            metric_from_console(tx_console, "stream_sender.peer_fec_packet_received");
        if (rx_pkts > 0 && peer_fec > 0)
        {
            ok = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_TRUE(ok) << "expected media flow and reverse telemetry within 5s";

    stop_process(snd_pid);
    stop_process(rcv_pid);
}
