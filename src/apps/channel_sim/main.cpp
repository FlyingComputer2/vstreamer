/*
 * channel_sim: UDP channel simulator between a stream_sender and a stream_receiver. Datagrams
 * arriving on --listen go to --forward through a link_emulator (throughput cap, constant random
 * loss, Gilbert-Elliott burst loss); the receiver's replies go back to the last sender. The
 * impairment is set at startup with flags and changed at runtime over the UDP mplane.
 */

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>

#include "apps/channel_sim/channel_mplane.hpp"
#include "apps/common/app_console.hpp"
#include "apps/common/link_emulator.hpp"
#include "core/key_util.hpp"

namespace
{

constexpr const char k_default_listen[] = "0.0.0.0:5000";
constexpr const char k_default_mplane[] = "127.0.0.1:5095";

std::atomic<bool> g_run {true};

void on_signal(int /*sig*/)
{
    g_run.store(false);
}

void usage(const char *argv0)
{
    std::fprintf(stderr,
                 "usage: %s --forward <host:port> [options]\n"
                 "  --listen <[host:]port>  ingress from the sender (default %s)\n"
                 "  --forward <host:port>   egress to the receiver\n"
                 "  --mplane <host:port>    UDP management plane (default %s; no\n"
                 "                          authentication, keep it on loopback)\n"
                 "  --no-mplane             do not open the management plane\n"
                 "  --loss <pct>            constant random loss\n"
                 "  --ge <p_gb,p_bg[,loss_bad[,loss_good]]>\n"
                 "                          Gilbert-Elliott burst loss, all in %%\n"
                 "  --max-kbps <kbps>       throughput cap (0 = none)\n"
                 "  --queue <n>             ingress queue depth (default %d)\n"
                 "  --seed <n>              loss generator seed\n"
                 "  --stats <s>             print counters every s seconds (0 = off)\n",
                 argv0, k_default_listen, k_default_mplane,
                 vstreamer::apps::k_link_default_queue_depth);
}

/* "[host:]port" with port 1..65535; a bare port keeps *host. */
bool parse_host_port(std::string_view s, std::string *host, int *port, bool host_required)
{
    const size_t     colon = s.rfind(':');
    std::string_view p = s;
    if (std::string_view::npos != colon)
    {
        if (0 == colon)
        {
            return false;
        }
        *host = std::string(s.substr(0, colon));
        p = s.substr(colon + 1);
    }
    else if (host_required)
    {
        return false;
    }
    int64_t v = 0;
    if (vstreamer::key_parse_i64(p, &v) != 0 || v < 1 || v > 65535)
    {
        return false;
    }
    *port = static_cast<int>(v);
    return true;
}

bool parse_ge(std::string_view s, vstreamer::apps::gilbert_elliott *ge)
{
    double vals[4] = {0., 0., 100., 0.};
    int    n = 0;
    while (!s.empty() && n < 4)
    {
        const size_t comma = s.find(',');
        if (vstreamer::key_parse_double(s.substr(0, comma), &vals[n]) != 0 || vals[n] < 0. ||
            vals[n] > 100.)
        {
            return false;
        }
        n++;
        s = std::string_view::npos == comma ? std::string_view() : s.substr(comma + 1);
    }
    if (n < 2 || !s.empty())
    {
        return false;
    }
    ge->p_gb = vals[0];
    ge->p_bg = vals[1];
    ge->loss_bad = vals[2];
    ge->loss_good = vals[3];
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    std::string listen_host = "0.0.0.0";
    int         listen_port = 0;
    std::string forward_host;
    int         forward_port = 0;
    std::string mplane_host;
    int         mplane_port = 0;
    bool        mplane_on = true;
    double      loss = 0.;
    double      max_kbps = 0.;
    int64_t     queue = vstreamer::apps::k_link_default_queue_depth;
    int64_t     seed = -1;
    int64_t     stats_s = 0;

    vstreamer::apps::gilbert_elliott ge;

    (void)parse_host_port(k_default_listen, &listen_host, &listen_port, true);
    (void)parse_host_port(k_default_mplane, &mplane_host, &mplane_port, true);

    for (int i = 1; i < argc; i++)
    {
        const std::string_view a = argv[i];
        const bool             has_val = i + 1 < argc;
        bool                   ok = true;
        if ("--listen" == a && has_val)
        {
            ok = parse_host_port(argv[++i], &listen_host, &listen_port, false);
        }
        else if ("--forward" == a && has_val)
        {
            ok = parse_host_port(argv[++i], &forward_host, &forward_port, true);
        }
        else if ("--mplane" == a && has_val)
        {
            ok = parse_host_port(argv[++i], &mplane_host, &mplane_port, true);
        }
        else if ("--no-mplane" == a)
        {
            mplane_on = false;
        }
        else if ("--loss" == a && has_val)
        {
            ok = vstreamer::key_parse_double(argv[++i], &loss) == 0 && loss >= 0. && loss <= 100.;
        }
        else if ("--ge" == a && has_val)
        {
            ok = parse_ge(argv[++i], &ge);
        }
        else if ("--max-kbps" == a && has_val)
        {
            ok = vstreamer::key_parse_double(argv[++i], &max_kbps) == 0 && max_kbps >= 0.;
        }
        else if ("--queue" == a && has_val)
        {
            ok = vstreamer::key_parse_i64(argv[++i], &queue) == 0 && queue >= 0 &&
                 queue <= 1'000'000;
        }
        else if ("--seed" == a && has_val)
        {
            ok = vstreamer::key_parse_i64(argv[++i], &seed) == 0 && seed >= 0 &&
                 seed <= UINT32_MAX;
        }
        else if ("--stats" == a && has_val)
        {
            ok = vstreamer::key_parse_i64(argv[++i], &stats_s) == 0 && stats_s >= 0;
        }
        else if ("-h" == a || "--help" == a)
        {
            usage(argv[0]);
            return 0;
        }
        else
        {
            ok = false;
        }
        if (!ok)
        {
            std::fprintf(stderr, "channel_sim: bad argument '%s'\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }
    if (forward_host.empty())
    {
        usage(argv[0]);
        return 2;
    }

    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    vstreamer::apps::link_emulator link;
    link.set_bind_host(listen_host.c_str());
    link.set_constant_loss(loss);
    link.set_gilbert_elliott(ge);
    link.set_max_kbps(max_kbps);
    link.set_queue_depth(static_cast<int>(queue));
    if (seed >= 0)
    {
        link.set_seed(static_cast<uint32_t>(seed));
    }
    int r = link.start(listen_port, forward_host.c_str(), forward_port);
    if (r < 0)
    {
        std::fprintf(stderr, "channel_sim: %s:%d -> %s:%d failed (%s)\n", listen_host.c_str(),
                     listen_port, forward_host.c_str(), forward_port, std::strerror(-r));
        return 1;
    }

    vstreamer::apps::app_console mplane;
    if (mplane_on)
    {
        mplane.set_bind_host(mplane_host.c_str());
        mplane.add_handler(
            [&link](const char *line, std::string &reply) {
                return vstreamer::apps::handle_channel_mplane_line(link, line, reply);
            },
            vstreamer::apps::channel_mplane_help());
        r = mplane.start(mplane_port);
        if (r < 0)
        {
            std::fprintf(stderr, "channel_sim: mplane bind %s:%d failed (%s)\n",
                         mplane_host.c_str(), mplane_port, std::strerror(-r));
            return 1;
        }
        std::fprintf(stderr, "channel_sim: mplane on udp %s:%d\n", mplane_host.c_str(),
                     mplane_port);
    }

    std::string config;
    (void)vstreamer::apps::handle_channel_mplane_line(link, "get link config", config);
    std::fprintf(stderr, "channel_sim: %s", config.c_str());

    auto next_stats = std::chrono::steady_clock::now() + std::chrono::seconds(stats_s);
    while (g_run.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (stats_s > 0 && std::chrono::steady_clock::now() >= next_stats)
        {
            next_stats += std::chrono::seconds(stats_s);
            std::string stats;
            (void)vstreamer::apps::handle_channel_mplane_line(link, "get link stats", stats);
            std::fprintf(stderr, "channel_sim: %s", stats.c_str());
        }
    }
    mplane.stop();
    link.stop();
    return 0;
}
