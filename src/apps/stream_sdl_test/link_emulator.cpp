#include "apps/stream_sdl_test/link_emulator.hpp"

#include "components/stream_sender.hpp"
#include "core/component_coder.hpp"
#include "core/metrics.hpp"

#include <cerrno>
#include <cinttypes>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>

namespace vstreamer::test_app
{
namespace
{

double now_sec()
{
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

uint32_t rng32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x != 0 ? x : 1;
    return *state;
}

int bind_ipv4_host(sockaddr_in *in, const std::string &host, int port)
{
    if (nullptr == in)
    {
        return -EINVAL;
    }
    std::memset(in, 0, sizeof(*in));
    in->sin_family = AF_INET;
    in->sin_port = htons(static_cast<uint16_t>(port));
    if (host == "0.0.0.0" || host == "*")
    {
        in->sin_addr.s_addr = htonl(INADDR_ANY);
        return 0;
    }
    if (inet_pton(AF_INET, host.c_str(), &in->sin_addr) != 1)
    {
        return -EINVAL;
    }
    return 0;
}

}  // namespace

link_emulator::link_emulator() = default;

link_emulator::~link_emulator()
{
    stop();
}

void link_emulator::reset_rate_windows(double t)
{
    {
        std::lock_guard<std::mutex> rlock(fwd.rate_mu);
        fwd.rate_window_start = t;
        fwd.rate_window_bytes = 0;
    }
    {
        std::lock_guard<std::mutex> rlock(rev.rate_mu);
        rev.rate_window_start = t;
        rev.rate_window_bytes = 0;
    }
}

void link_emulator::set_max_kbps(double kbps)
{
    {
        std::lock_guard<std::mutex> lock(cfg_mu);
        max_kbps_limit = kbps < 0. ? 0. : kbps;
    }
    reset_rate_windows(now_sec());
}

void link_emulator::set_drop_dt_ms(int ms)
{
    {
        std::lock_guard<std::mutex> lock(cfg_mu);
        if (ms < k_chan_min_drop_dt_ms)
        {
            rate_drop_dt_ms = k_chan_min_drop_dt_ms;
        }
        else if (ms > k_chan_max_drop_dt_ms)
        {
            rate_drop_dt_ms = k_chan_max_drop_dt_ms;
        }
        else
        {
            rate_drop_dt_ms = ms;
        }
    }
    reset_rate_windows(now_sec());
}

void link_emulator::set_constant_loss(double pct)
{
    std::lock_guard<std::mutex> lock(cfg_mu);
    if (pct < 0.)
    {
        loss_pct = 0.;
    }
    else if (pct > 100.)
    {
        loss_pct = 100.;
    }
    else
    {
        loss_pct = pct;
    }
}

void link_emulator::set_queue_depth(int depth)
{
    std::lock_guard<std::mutex> lock(cfg_mu);
    if (depth < 0)
    {
        ingress_queue_depth = 0;
        return;
    }
    ingress_queue_depth = static_cast<size_t>(depth);
}

double link_emulator::max_kbps() const
{
    std::lock_guard<std::mutex> lock(cfg_mu);
    return max_kbps_limit;
}

int link_emulator::drop_dt_ms() const
{
    std::lock_guard<std::mutex> lock(cfg_mu);
    return rate_drop_dt_ms;
}

double link_emulator::constant_loss() const
{
    std::lock_guard<std::mutex> lock(cfg_mu);
    return loss_pct;
}

int link_emulator::queue_depth() const
{
    std::lock_guard<std::mutex> lock(cfg_mu);
    return static_cast<int>(ingress_queue_depth);
}

size_t link_emulator::forward_queue_size() const
{
    return fwd.ingress_queue_len.load(std::memory_order_relaxed);
}

link_emulator::forward_stats link_emulator::forward_stats_snapshot() const
{
    forward_stats s;
    s.pkts_in = fwd.pkts_in.load(std::memory_order_relaxed);
    s.pkts_out = fwd.pkts_out.load(std::memory_order_relaxed);
    s.bytes_in = fwd.bytes_in.load(std::memory_order_relaxed);
    s.bytes_out = fwd.bytes_out.load(std::memory_order_relaxed);
    s.dropped_rate = fwd.dropped_rate.load(std::memory_order_relaxed);
    s.dropped_loss = fwd.dropped_loss.load(std::memory_order_relaxed);
    s.dropped_queue = fwd.dropped_queue.load(std::memory_order_relaxed);
    return s;
}

link_emulator::forward_stats link_emulator::reverse_stats_snapshot() const
{
    forward_stats s;
    s.pkts_in = rev.pkts_in.load(std::memory_order_relaxed);
    s.pkts_out = rev.pkts_out.load(std::memory_order_relaxed);
    s.bytes_in = rev.bytes_in.load(std::memory_order_relaxed);
    s.bytes_out = rev.bytes_out.load(std::memory_order_relaxed);
    s.dropped_rate = rev.dropped_rate.load(std::memory_order_relaxed);
    s.dropped_loss = rev.dropped_loss.load(std::memory_order_relaxed);
    s.dropped_queue = rev.dropped_queue.load(std::memory_order_relaxed);
    return s;
}

bool link_emulator::should_drop_rate(direction_state &dir, size_t pkt_bytes)
{
    double limit = 0.;
    double window_sec = 0.;
    {
        std::lock_guard<std::mutex> lock(cfg_mu);
        limit = max_kbps_limit;
        window_sec = static_cast<double>(rate_drop_dt_ms) / 1000.0;
    }
    if (limit <= 0.)
    {
        return false;
    }

    const double t = now_sec();
    std::lock_guard<std::mutex> lock(dir.rate_mu);
    if (dir.rate_window_start <= 0. || (t - dir.rate_window_start) >= window_sec)
    {
        dir.rate_window_start = t;
        dir.rate_window_bytes = 0;
    }

    const double max_bytes = limit * 1000.0 / 8.0 * window_sec;
    if (static_cast<double>(dir.rate_window_bytes + pkt_bytes) > max_bytes)
    {
        return true;
    }
    dir.rate_window_bytes += static_cast<uint64_t>(pkt_bytes);
    return false;
}

bool link_emulator::should_drop_loss(direction_state &dir)
{
    double pct = 0.;
    {
        std::lock_guard<std::mutex> lock(cfg_mu);
        pct = loss_pct;
    }
    if (pct <= 0.)
    {
        return false;
    }
    if (pct >= 100.)
    {
        return true;
    }
    const uint32_t r = rng32(&dir.rng);
    const double   u = static_cast<double>(r % 10000) / 10000.0;
    return u < (pct / 100.0);
}

link_emulator::egress_status link_emulator::try_egress_one(direction_state &dir,
                                                                     const uint8_t *buf, size_t n)
{
    if (should_drop_rate(dir, n))
    {
        return egress_status::rate_limited;
    }
    if (should_drop_loss(dir))
    {
        return egress_status::loss_dropped;
    }

    if (!dir.have_egress || dir.egress_fd < 0)
    {
        return egress_status::no_route;
    }

    const ssize_t sent =
        sendto(dir.egress_fd, buf, n, 0, reinterpret_cast<sockaddr *>(&dir.egress_addr),
               sizeof(dir.egress_addr));
    if (sent == static_cast<ssize_t>(n))
    {
        dir.pkts_out.fetch_add(1, std::memory_order_relaxed);
        dir.bytes_out.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
        return egress_status::ok;
    }
    return egress_status::send_failed;
}

void link_emulator::egress_packet(direction_state &dir, const uint8_t *buf, size_t n)
{
    switch (try_egress_one(dir, buf, n))
    {
    case egress_status::rate_limited:
        dir.dropped_rate.fetch_add(1, std::memory_order_relaxed);
        break;
    case egress_status::loss_dropped:
        dir.dropped_loss.fetch_add(1, std::memory_order_relaxed);
        break;
    case egress_status::ok:
    case egress_status::no_route:
    case egress_status::send_failed:
        break;
    }
}

void link_emulator::accept_ingress(direction_state &dir, const uint8_t *buf, size_t n)
{
    dir.pkts_in.fetch_add(1, std::memory_order_relaxed);
    dir.bytes_in.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);

    size_t depth = 0;
    {
        std::lock_guard<std::mutex> lock(cfg_mu);
        depth = ingress_queue_depth;
    }
    if (0 == depth)
    {
        egress_packet(dir, buf, n);
        return;
    }

    if (dir.ingress_queue.size() >= depth)
    {
        dir.ingress_queue.pop_front();
        dir.dropped_queue.fetch_add(1, std::memory_order_relaxed);
    }
    dir.ingress_queue.emplace_back(buf, buf + n);
    dir.ingress_queue_len.store(dir.ingress_queue.size(), std::memory_order_relaxed);
}

void link_emulator::flush_ingress_queue(direction_state &dir)
{
    size_t depth = 0;
    {
        std::lock_guard<std::mutex> lock(cfg_mu);
        depth = ingress_queue_depth;
    }
    if (0 == depth)
    {
        return;
    }

    bool hold_remainder = false;
    while (!hold_remainder && !dir.ingress_queue.empty())
    {
        const std::vector<uint8_t> &pkt = dir.ingress_queue.front();
        switch (try_egress_one(dir, pkt.data(), pkt.size()))
        {
        case egress_status::ok:
            dir.ingress_queue.pop_front();
            break;
        case egress_status::loss_dropped:
            dir.ingress_queue.pop_front();
            dir.dropped_loss.fetch_add(1, std::memory_order_relaxed);
            break;
        case egress_status::rate_limited:
        case egress_status::no_route:
        case egress_status::send_failed:
            hold_remainder = true;
            break;
        }
    }
    dir.ingress_queue_len.store(dir.ingress_queue.size(), std::memory_order_relaxed);
}

void link_emulator::relay_thread_main()
{
    uint8_t buf[2048];

    while (!relay_stop.load())
    {
        pollfd fds[2];
        int    nfds = 0;

        if (fwd.ingress_fd >= 0)
        {
            fds[nfds].fd = fwd.ingress_fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }
        if (rev_enabled && rev.ingress_fd >= 0)
        {
            fds[nfds].fd = rev.ingress_fd;
            fds[nfds].events = POLLIN;
            nfds++;
        }

        if (nfds == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        const int pr = poll(fds, nfds, 50);
        if (pr < 0)
        {
            if (relay_stop.load())
            {
                break;
            }
            continue;
        }
        auto drain_ingress = [&](direction_state &dir) {
            while (!relay_stop.load())
            {
                const ssize_t n =
                    recv(dir.ingress_fd, buf, sizeof(buf), MSG_DONTWAIT);
                if (n > 0)
                {
                    accept_ingress(dir, buf, static_cast<size_t>(n));
                    flush_ingress_queue(dir);
                    continue;
                }
                if (n < 0 && (EAGAIN == errno || EWOULDBLOCK == errno))
                {
                    break;
                }
                break;
            }
        };

        int idx = 0;
        if (fwd.ingress_fd >= 0)
        {
            if (pr != 0 && (fds[idx].revents & POLLIN) != 0)
            {
                drain_ingress(fwd);
            }
            idx++;
        }
        if (rev_enabled && rev.ingress_fd >= 0)
        {
            if (pr != 0 && (fds[idx].revents & POLLIN) != 0)
            {
                drain_ingress(rev);
            }
        }

        flush_ingress_queue(fwd);
        if (rev_enabled)
        {
            flush_ingress_queue(rev);
        }
    }
}

void link_emulator::set_bind_host(const char *host)
{
    if (nullptr == host || host[0] == '\0')
    {
        bind_host = k_loopback_host;
        return;
    }
    bind_host = host;
}

int link_emulator::setup_direction(direction_state &dir, int ingress_port,
                                      const char *egress_host, int egress_port)
{
    dir.ingress_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (dir.ingress_fd < 0)
    {
        return -errno;
    }

    sockaddr_in in {};
    const int   bind_r = bind_ipv4_host(&in, bind_host, ingress_port);
    if (bind_r < 0)
    {
        ::close(dir.ingress_fd);
        dir.ingress_fd = -1;
        return bind_r;
    }
    if (bind(dir.ingress_fd, reinterpret_cast<sockaddr *>(&in), sizeof(in)) < 0)
    {
        const int err = -errno;
        ::close(dir.ingress_fd);
        dir.ingress_fd = -1;
        return err;
    }

    constexpr int k_sock_buf = 16 * 1024 * 1024;
    (void)setsockopt(dir.ingress_fd, SOL_SOCKET, SO_RCVBUF, &k_sock_buf, sizeof(k_sock_buf));

    dir.egress_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (dir.egress_fd < 0)
    {
        const int err = -errno;
        ::close(dir.ingress_fd);
        dir.ingress_fd = -1;
        return err;
    }

    std::memset(&dir.egress_addr, 0, sizeof(dir.egress_addr));
    dir.egress_addr.sin_family = AF_INET;
    dir.egress_addr.sin_port = htons(static_cast<uint16_t>(egress_port));
    if (inet_pton(AF_INET, egress_host, &dir.egress_addr.sin_addr) != 1)
    {
        ::close(dir.egress_fd);
        dir.egress_fd = -1;
        ::close(dir.ingress_fd);
        dir.ingress_fd = -1;
        return -EINVAL;
    }
    (void)setsockopt(dir.egress_fd, SOL_SOCKET, SO_SNDBUF, &k_sock_buf, sizeof(k_sock_buf));

    dir.have_egress = true;
    return 0;
}

void link_emulator::teardown_direction(direction_state &dir)
{
    dir.ingress_queue.clear();
    dir.ingress_queue_len.store(0, std::memory_order_relaxed);
    if (dir.ingress_fd >= 0)
    {
        ::shutdown(dir.ingress_fd, SHUT_RDWR);
        ::close(dir.ingress_fd);
        dir.ingress_fd = -1;
    }
    if (dir.egress_fd >= 0)
    {
        ::close(dir.egress_fd);
        dir.egress_fd = -1;
    }
    dir.have_egress = false;
}

int link_emulator::start(int ingress_port, const char *egress_host, int egress_port,
                              int reverse_ingress_port, const char *reverse_egress_host,
                              int reverse_egress_port)
{
    stop_relay();

    const int ret = setup_direction(fwd, ingress_port, egress_host, egress_port);
    if (ret < 0)
    {
        return ret;
    }

    rev_enabled = false;
    if (reverse_ingress_port > 0 && nullptr != reverse_egress_host && reverse_egress_port > 0)
    {
        const int rret =
            setup_direction(rev, reverse_ingress_port, reverse_egress_host, reverse_egress_port);
        if (rret < 0)
        {
            teardown_direction(fwd);
            return rret;
        }
        rev_enabled = true;
        rev.rng = 0xA5A5A5A5u;
    }

    relay_stop = false;
    relay_thread = std::thread(&link_emulator::relay_thread_main, this);

    std::fprintf(stderr, "channel_controller: fwd :%d -> %s:%d (queue=%d)\n", ingress_port,
                 egress_host, egress_port, queue_depth());
    if (rev_enabled)
    {
        std::fprintf(stderr, "channel_controller: rev :%d -> %s:%d\n", reverse_ingress_port,
                     reverse_egress_host, reverse_egress_port);
    }
    return 0;
}

void link_emulator::stop_relay()
{
    relay_stop = true;
    if (fwd.ingress_fd >= 0)
    {
        ::shutdown(fwd.ingress_fd, SHUT_RDWR);
    }
    if (rev_enabled && rev.ingress_fd >= 0)
    {
        ::shutdown(rev.ingress_fd, SHUT_RDWR);
    }

    if (relay_thread.joinable())
    {
        relay_thread.join();
    }

    teardown_direction(fwd);
    if (rev_enabled)
    {
        teardown_direction(rev);
    }
    rev_enabled = false;
    relay_stop = false;
}

void link_emulator::stop()
{
    stop_relay();
}

}  // namespace vstreamer::test_app
