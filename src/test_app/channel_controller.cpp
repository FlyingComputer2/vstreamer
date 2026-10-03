#include "test_app/channel_controller.hpp"

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

void trim_inplace(char *s)
{
    if (nullptr == s)
    {
        return;
    }
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r')
    {
        start++;
    }
    if (start != s)
    {
        std::memmove(s, start, std::strlen(start) + 1);
    }
    size_t len = std::strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r'))
    {
        s[len - 1] = '\0';
        len--;
    }
}

}  // namespace

channel_controller::channel_controller() = default;

channel_controller::~channel_controller()
{
    stop();
}

void channel_controller::set_stream_sender(vstreamer::stream_sender *sender)
{
    stream_tx = sender;
}

void channel_controller::reset_rate_windows(double t)
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

void channel_controller::set_max_kbps(double kbps)
{
    {
        std::lock_guard<std::mutex> lock(cfg_mu);
        max_kbps_limit = kbps < 0. ? 0. : kbps;
    }
    reset_rate_windows(now_sec());
}

void channel_controller::set_drop_dt_ms(int ms)
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

void channel_controller::set_constant_loss(double pct)
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

void channel_controller::set_queue_depth(int depth)
{
    std::lock_guard<std::mutex> lock(cfg_mu);
    if (depth < 0)
    {
        ingress_queue_depth = 0;
        return;
    }
    ingress_queue_depth = static_cast<size_t>(depth);
}

double channel_controller::max_kbps() const
{
    std::lock_guard<std::mutex> lock(cfg_mu);
    return max_kbps_limit;
}

int channel_controller::drop_dt_ms() const
{
    std::lock_guard<std::mutex> lock(cfg_mu);
    return rate_drop_dt_ms;
}

double channel_controller::constant_loss() const
{
    std::lock_guard<std::mutex> lock(cfg_mu);
    return loss_pct;
}

int channel_controller::queue_depth() const
{
    std::lock_guard<std::mutex> lock(cfg_mu);
    return static_cast<int>(ingress_queue_depth);
}

size_t channel_controller::forward_queue_size() const
{
    return fwd.ingress_queue_len.load(std::memory_order_relaxed);
}

channel_controller::forward_stats channel_controller::forward_stats_snapshot() const
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

bool channel_controller::should_drop_rate(direction_state &dir, size_t pkt_bytes)
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

bool channel_controller::should_drop_loss(direction_state &dir)
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

channel_controller::egress_status channel_controller::try_egress_one(direction_state &dir,
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

void channel_controller::egress_packet(direction_state &dir, const uint8_t *buf, size_t n)
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

void channel_controller::accept_ingress(direction_state &dir, const uint8_t *buf, size_t n)
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

void channel_controller::flush_ingress_queue(direction_state &dir)
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

void channel_controller::relay_thread_main()
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

void channel_controller::set_pipeline_metrics(const vstreamer::metrics *source)
{
    pipeline_metrics = source;
}

void channel_controller::set_pipeline_metrics_refresh(std::function<void()> refresh)
{
    pipeline_metrics_refresh = std::move(refresh);
}

void channel_controller::set_pipeline_metrics_sync_live(std::function<void()> sync_live)
{
    pipeline_metrics_sync_live = std::move(sync_live);
}

void channel_controller::set_source_state_metrics_refresh(std::function<void()> refresh)
{
    source_state_metrics_refresh = std::move(refresh);
}

void channel_controller::set_encode_target(vstreamer::component_coder *encoder)
{
    encode_target = encoder;
}

void channel_controller::set_encode_command_handlers(std::function<bool(int kbps)> set_cbr_kbps,
                                                     std::function<bool(int qp)> set_qp,
                                                     std::function<bool(int gop)> set_gop,
                                                     std::function<bool()> force_idr)
{
    encode_set_cbr_kbps = std::move(set_cbr_kbps);
    encode_set_qp = std::move(set_qp);
    encode_set_gop = std::move(set_gop);
    encode_force_idr = std::move(force_idr);
}

void channel_controller::send_pipeline_metrics(int reply_fd, const sockaddr_in &reply)
{
    if (nullptr == pipeline_metrics)
    {
        const char *msg = "err metrics not configured\n";
        sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
               sizeof(reply));
        return;
    }

    /* Live snapshot: fast counter sync, then copy/format metrics (not full update_pipeline_metrics). */
    if (pipeline_metrics_sync_live)
    {
        pipeline_metrics_sync_live();
    }
    else if (source_state_metrics_refresh)
    {
        source_state_metrics_refresh();
    }
    const std::string report = pipeline_metrics->to_string();
    if (report.empty())
    {
        const char *msg = "err metrics empty\n";
        sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
               sizeof(reply));
        return;
    }

    sendto(reply_fd, report.data(), report.size(), 0, reinterpret_cast<const sockaddr *>(&reply),
           sizeof(reply));
}

void channel_controller::handle_console_line(const char *line, int reply_fd,
                                             const sockaddr_in &reply)
{
    char work[256];
    std::snprintf(work, sizeof(work), "%s", line);
    trim_inplace(work);

    if (0 == std::strcmp(work, "help") || 0 == std::strcmp(work, "h") ||
        0 == std::strcmp(work, "?"))
    {
        static const char help_msg[] =
            "set_max_kbps <kbps>\n"
            "set_drop_dt_ms <ms>\n"
            "set_constant_loss <pct>\n"
            "set_fec none\n"
            "set_fec_k <k>\n"
            "set_fec_n <n>\n"
            "set_encode_cbr <kbps>\n"
            "set_encode_qp <qp>\n"
            "set_gop <gop>\n"
            "force_idr\n"
            "get_metric <metric_name>\n"
            "ping\n"
            "stats\n"
            "metrics\n"
            "get\n"
            "(empty line)  pipeline metrics\n";
        sendto(reply_fd, help_msg, std::strlen(help_msg), 0,
               reinterpret_cast<const sockaddr *>(&reply), sizeof(reply));
        return;
    }

    if (0 == std::strcmp(work, "ping"))
    {
        const char *msg = "pong\n";
        sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
               sizeof(reply));
        return;
    }

    if (0 == std::strncmp(work, "set_max_kbps ", 13))
    {
        const char *arg = work + 13;
        char       *end = nullptr;
        const double v = strtod(arg, &end);
        if (end == arg)
        {
            const char *msg = "err bad value\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        set_max_kbps(v);
        const char *msg = "ok\n";
        sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
               sizeof(reply));
        return;
    }

    if (0 == std::strncmp(work, "set_drop_dt_ms ", 15))
    {
        const char *arg = work + 15;
        char       *end = nullptr;
        const long  v = std::strtol(arg, &end, 10);
        if (end == arg || v < k_chan_min_drop_dt_ms || v > k_chan_max_drop_dt_ms)
        {
            const char *msg = "err bad value\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        set_drop_dt_ms(static_cast<int>(v));
        const char *msg = "ok\n";
        sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
               sizeof(reply));
        return;
    }

    if (0 == std::strncmp(work, "set_constant_loss ", 18))
    {
        const char *arg = work + 18;
        char       *end = nullptr;
        const double v = strtod(arg, &end);
        if (end == arg)
        {
            const char *msg = "err bad value\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        set_constant_loss(v);
        const char *msg = "ok\n";
        sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
               sizeof(reply));
        return;
    }

    if (0 == std::strcmp(work, "set_fec none"))
    {
        if (nullptr == stream_tx)
        {
            const char *msg = "err stream_sender not configured\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        static const char none_mode[] = "none";
        std::string_view val = none_mode;
        if (stream_tx->configure("fec", val) < 0)
        {
            const char *msg = "err set_fec none\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        const char *msg = "ok\n";
        sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
               sizeof(reply));
        return;
    }

    if (0 == std::strncmp(work, "set_fec_k ", 10))
    {
        if (nullptr == stream_tx)
        {
            const char *msg = "err stream_sender not configured\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        const char *arg = work + 10;
        char       *end = nullptr;
        const long  k = std::strtol(arg, &end, 10);
        if (end == arg || k < 1 || k > 15)
        {
            const char *msg = "err bad k (1..15)\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%ld", k);
        std::string_view val = buf;
        if (stream_tx->configure("fec_k", val) < 0)
        {
            const char *msg = "err bad k (1..15)\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        const char *msg = "ok\n";
        sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
               sizeof(reply));
        return;
    }

    if (0 == std::strncmp(work, "set_fec_n ", 10))
    {
        if (nullptr == stream_tx)
        {
            const char *msg = "err stream_sender not configured\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        const char *arg = work + 10;
        char       *end = nullptr;
        const long  n = std::strtol(arg, &end, 10);
        if (end == arg || n < 1 || n > 15)
        {
            const char *msg = "err bad n (k..15)\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%ld", n);
        std::string_view val = buf;
        if (stream_tx->configure("fec_n", val) < 0)
        {
            const char *msg = "err bad n (k..15)\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        const char *msg = "ok\n";
        sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
               sizeof(reply));
        return;
    }

    if (0 == std::strncmp(work, "set_encode_cbr ", 15))
    {
        const char *arg = work + 15;
        char       *end = nullptr;
        const long  kbps = std::strtol(arg, &end, 10);
        if (end == arg || kbps < 100 || kbps > 200'000)
        {
            const char *msg = "err bad kbps (100..200000)\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        bool ok = false;
        if (encode_set_cbr_kbps)
        {
            ok = encode_set_cbr_kbps(static_cast<int>(kbps));
        }
        else if (nullptr != encode_target)
        {
            char bps_buf[32];
            std::snprintf(bps_buf, sizeof(bps_buf), "%ld", kbps * 1000L);
            std::string_view val = bps_buf;
            ok = encode_target->configure("cbr", val) == 0;
        }
        if (!ok)
        {
            const char *msg = encode_set_cbr_kbps || nullptr != encode_target
                                  ? "err set cbr failed\n"
                                  : "err encoder not configured\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        const char *msg = "ok\n";
        sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
               sizeof(reply));
        return;
    }

    if (0 == std::strncmp(work, "set_gop ", 8))
    {
        const char *arg = work + 8;
        char       *end = nullptr;
        const long  gop = std::strtol(arg, &end, 10);
        if (end == arg || gop < 1 || gop > 255)
        {
            const char *msg = "err bad gop (1..255)\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        bool ok = false;
        if (encode_set_gop)
        {
            ok = encode_set_gop(static_cast<int>(gop));
        }
        else if (nullptr != encode_target)
        {
            char gop_buf[16];
            std::snprintf(gop_buf, sizeof(gop_buf), "%ld", gop);
            std::string_view val = gop_buf;
            ok = encode_target->configure("gop", val) == 0;
        }
        if (!ok)
        {
            const char *msg = encode_set_gop || nullptr != encode_target ? "err set gop failed\n"
                                                                         : "err encoder not configured\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        const char *msg = "ok\n";
        sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
               sizeof(reply));
        return;
    }

    if (0 == std::strcmp(work, "force_idr"))
    {
        bool ok = false;
        if (encode_force_idr)
        {
            ok = encode_force_idr();
        }
        else if (nullptr != encode_target)
        {
            ok = encode_target->configure("idr", "") == 0;
        }
        if (!ok)
        {
            const char *msg = encode_force_idr || nullptr != encode_target
                                  ? "err force_idr failed\n"
                                  : "err encoder not configured\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        const char *msg = "ok\n";
        sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
               sizeof(reply));
        return;
    }

    if (0 == std::strncmp(work, "set_encode_qp ", 14))
    {
        const char *arg = work + 14;
        char       *end = nullptr;
        const long  qp = std::strtol(arg, &end, 10);
        if (end == arg || qp < 0 || qp > 51)
        {
            const char *msg = "err bad qp (0..51)\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        bool ok = false;
        if (encode_set_qp)
        {
            ok = encode_set_qp(static_cast<int>(qp));
        }
        else if (nullptr != encode_target)
        {
            char qp_buf[16];
            std::snprintf(qp_buf, sizeof(qp_buf), "%ld", qp);
            std::string_view val = qp_buf;
            ok = encode_target->configure("qp", val) == 0;
        }
        if (!ok)
        {
            const char *msg = encode_set_qp || nullptr != encode_target ? "err set qp failed\n"
                                                                        : "err encoder not configured\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        const char *msg = "ok\n";
        sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
               sizeof(reply));
        return;
    }

    if (0 == std::strncmp(work, "get_metric ", 11))
    {
        char *name = work + 11;
        trim_inplace(name);
        if ('\0' == name[0])
        {
            const char *msg = "err metric name required\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        if (nullptr == pipeline_metrics)
        {
            const char *msg = "err metrics not configured\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        if (0 == std::strcmp(name, "source.state") && source_state_metrics_refresh)
        {
            source_state_metrics_refresh();
        }
        std::string value;
        if (!pipeline_metrics->format_metric(name, &value))
        {
            const char *msg = "err unknown metric\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        value.push_back('\n');
        sendto(reply_fd, value.data(), value.size(), 0, reinterpret_cast<const sockaddr *>(&reply),
               sizeof(reply));
        return;
    }

    if (0 == std::strcmp(work, "stats"))
    {
        char msg[512];
        std::snprintf(msg, sizeof(msg),
                      "fwd in=%" PRIu64 " out=%" PRIu64 " drop_rate=%" PRIu64
                      " drop_loss=%" PRIu64 " drop_queue=%" PRIu64
                      " | rev in=%" PRIu64 " out=%" PRIu64 " drop_rate=%" PRIu64
                      " drop_loss=%" PRIu64 " drop_queue=%" PRIu64
                      " | max_kbps=%.0f drop_dt_ms=%d queue=%d loss_pct=%.2f\n",
                      fwd.pkts_in.load(std::memory_order_relaxed),
                      fwd.pkts_out.load(std::memory_order_relaxed),
                      fwd.dropped_rate.load(std::memory_order_relaxed),
                      fwd.dropped_loss.load(std::memory_order_relaxed),
                      fwd.dropped_queue.load(std::memory_order_relaxed),
                      rev.pkts_in.load(std::memory_order_relaxed),
                      rev.pkts_out.load(std::memory_order_relaxed),
                      rev.dropped_rate.load(std::memory_order_relaxed),
                      rev.dropped_loss.load(std::memory_order_relaxed),
                      rev.dropped_queue.load(std::memory_order_relaxed), max_kbps(), drop_dt_ms(),
                      queue_depth(), constant_loss());
        sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
               sizeof(reply));
        return;
    }

    if (0 == std::strcmp(work, "metrics") || 0 == std::strcmp(work, "get"))
    {
        send_pipeline_metrics(reply_fd, reply);
        return;
    }

    if ('\0' == work[0])
    {
        send_pipeline_metrics(reply_fd, reply);
        return;
    }

    const char *msg = "err unknown\n";
    sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
           sizeof(reply));
}

void channel_controller::console_thread_main()
{
    uint8_t buf[512];
    while (!console_stop.load())
    {
        if (console_fd < 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        sockaddr_in from {};
        socklen_t   from_len = sizeof(from);
        const ssize_t n =
            recvfrom(console_fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr *>(&from),
                     &from_len);
        if (n <= 0)
        {
            if (console_stop.load())
            {
                break;
            }
            continue;
        }
        buf[n] = '\0';

        char *nl = static_cast<char *>(std::memchr(buf, '\n', static_cast<size_t>(n)));
        if (nullptr != nl)
        {
            *nl = '\0';
        }
        handle_console_line(reinterpret_cast<char *>(buf), console_fd, from);
    }
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

void channel_controller::set_bind_host(const char *host)
{
    if (nullptr == host || host[0] == '\0')
    {
        bind_host = k_loopback_host;
        return;
    }
    bind_host = host;
}

int channel_controller::setup_direction(direction_state &dir, int ingress_port,
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

void channel_controller::teardown_direction(direction_state &dir)
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

int channel_controller::start(int ingress_port, const char *egress_host, int egress_port,
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
    relay_thread = std::thread(&channel_controller::relay_thread_main, this);

    std::fprintf(stderr, "channel_controller: fwd :%d -> %s:%d (queue=%d)\n", ingress_port,
                 egress_host, egress_port, queue_depth());
    if (rev_enabled)
    {
        std::fprintf(stderr, "channel_controller: rev :%d -> %s:%d\n", reverse_ingress_port,
                     reverse_egress_host, reverse_egress_port);
    }
    return 0;
}

int channel_controller::start_console(int console_port)
{
    stop_console();

    console_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (console_fd < 0)
    {
        return -errno;
    }

    sockaddr_in in {};
    const int   bind_r = bind_ipv4_host(&in, bind_host, console_port);
    if (bind_r < 0)
    {
        ::close(console_fd);
        console_fd = -1;
        return bind_r;
    }
    if (bind(console_fd, reinterpret_cast<sockaddr *>(&in), sizeof(in)) < 0)
    {
        const int err = -errno;
        ::close(console_fd);
        console_fd = -1;
        return err;
    }

    console_stop = false;
    console_thread = std::thread(&channel_controller::console_thread_main, this);
    std::fprintf(stderr, "channel_controller: console udp :%d\n", console_port);
    return 0;
}

void channel_controller::stop_relay()
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

void channel_controller::stop_console()
{
    console_stop = true;
    if (console_fd >= 0)
    {
        ::shutdown(console_fd, SHUT_RDWR);
    }
    if (console_thread.joinable())
    {
        console_thread.join();
    }
    if (console_fd >= 0)
    {
        ::close(console_fd);
        console_fd = -1;
    }
    console_stop = false;
}

void channel_controller::stop()
{
    stop_console();
    stop_relay();
}

}  // namespace vstreamer::test_app
