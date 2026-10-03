#include "test_app/stream_sdl/bench_console.hpp"

#include "test_app/stream_sdl/channel_ports.hpp"
#include "test_app/stream_sdl/link_emulator.hpp"

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

void bench_console::set_bind_host(const char *host)
{
    if (nullptr == host || host[0] == '\0')
    {
        bind_host = k_loopback_host;
        return;
    }
    bind_host = host;
}

void bench_console::set_stream_sender(vstreamer::stream_sender *sender)
{
    stream_tx = sender;
}

void bench_console::set_pipeline_metrics(const vstreamer::metrics *source)
{
    pipeline_metrics = source;
}

void bench_console::set_pipeline_metrics_refresh(std::function<void()> refresh)
{
    pipeline_metrics_refresh = std::move(refresh);
}

void bench_console::set_pipeline_metrics_sync_live(std::function<void()> sync_live)
{
    pipeline_metrics_sync_live = std::move(sync_live);
}

void bench_console::set_source_state_metrics_refresh(std::function<void()> refresh)
{
    source_state_metrics_refresh = std::move(refresh);
}

void bench_console::set_encode_target(vstreamer::component_coder *encoder)
{
    encode_target = encoder;
}

void bench_console::set_encode_command_handlers(std::function<bool(int kbps)> set_cbr_kbps,
                                                     std::function<bool(int qp)> set_qp,
                                                     std::function<bool(int gop)> set_gop,
                                                     std::function<bool()> force_idr)
{
    encode_set_cbr_kbps = std::move(set_cbr_kbps);
    encode_set_qp = std::move(set_qp);
    encode_set_gop = std::move(set_gop);
    encode_force_idr = std::move(force_idr);
}

void bench_console::send_pipeline_metrics(int reply_fd, const sockaddr_in &reply)
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

void bench_console::handle_console_line(const char *line, int reply_fd,
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
        if (nullptr == link)
        {
            const char *msg = "err link not configured\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
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
        link->set_max_kbps(v);
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
        link->set_drop_dt_ms(static_cast<int>(v));
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
        link->set_constant_loss(v);
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
        if (nullptr == link)
        {
            const char *msg = "err link not configured\n";
            sendto(reply_fd, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr *>(&reply),
                   sizeof(reply));
            return;
        }
        const auto fwd = link->forward_stats_snapshot();
        const auto rev = link->reverse_stats_snapshot();
        char       msg[512];
        std::snprintf(msg, sizeof(msg),
                      "fwd in=%" PRIu64 " out=%" PRIu64 " drop_rate=%" PRIu64
                      " drop_loss=%" PRIu64 " drop_queue=%" PRIu64
                      " | rev in=%" PRIu64 " out=%" PRIu64 " drop_rate=%" PRIu64
                      " drop_loss=%" PRIu64 " drop_queue=%" PRIu64
                      " | max_kbps=%.0f drop_dt_ms=%d queue=%d loss_pct=%.2f\n",
                      fwd.pkts_in, fwd.pkts_out, fwd.dropped_rate, fwd.dropped_loss,
                      fwd.dropped_queue, rev.pkts_in, rev.pkts_out, rev.dropped_rate,
                      rev.dropped_loss, rev.dropped_queue, link->max_kbps(), link->drop_dt_ms(),
                      link->queue_depth(), link->constant_loss());
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

void bench_console::console_thread_main()
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

int bench_console::start(link_emulator &link_em, int console_port)
{
    link = &link_em;
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
    console_thread = std::thread(&bench_console::console_thread_main, this);
    std::fprintf(stderr, "channel_controller: console udp :%d\n", console_port);
    return 0;
}

void bench_console::stop_console()
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


bench_console::bench_console() = default;

bench_console::~bench_console()
{
    stop();
}

void bench_console::stop()
{
    stop_console();
}

}  // namespace vstreamer::test_app
