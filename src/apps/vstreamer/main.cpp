/* vstreamer: builds pipelines from a pipeline file and serves a UDP management plane. */

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>

#include "apps/common/app_console.hpp"
#include "apps/vstreamer/mplane.hpp"
#include "apps/vstreamer/pipeline_set.hpp"
#include "core/key_util.hpp"

namespace
{

constexpr const char k_default_mplane[] = "127.0.0.1:5090";
constexpr std::chrono::milliseconds k_restart_backoff {1000};

std::atomic<bool> g_run {true};

void on_signal(int /*sig*/)
{
    g_run.store(false);
}

void usage(const char *argv0)
{
    std::fprintf(stderr,
                 "usage: %s <pipeline-file> [--mplane <host:port>] [--no-mplane] [--check]\n"
                 "  --mplane     UDP management plane address (default %s; loopback only,\n"
                 "               there is no authentication)\n"
                 "  --no-mplane  do not open the management plane\n"
                 "  --check      load the file, report errors, and exit without starting\n",
                 argv0, k_default_mplane);
}

/* "host:port" with port 1..65535. */
bool parse_host_port(std::string_view s, std::string *host, int *port)
{
    const size_t colon = s.rfind(':');
    if (std::string_view::npos == colon || 0 == colon)
    {
        return false;
    }
    int64_t p = 0;
    if (vstreamer::key_parse_i64(s.substr(colon + 1), &p) != 0 || p < 1 || p > 65535)
    {
        return false;
    }
    *host = std::string(s.substr(0, colon));
    *port = static_cast<int>(p);
    return true;
}

}  // namespace

int main(int argc, char **argv)
{
    std::string file;
    std::string mplane_addr = k_default_mplane;
    bool        mplane_on = true;
    bool        check_only = false;
    for (int i = 1; i < argc; i++)
    {
        const std::string_view a = argv[i];
        if ("--mplane" == a && i + 1 < argc)
        {
            mplane_addr = argv[++i];
        }
        else if ("--no-mplane" == a)
        {
            mplane_on = false;
        }
        else if ("--check" == a)
        {
            check_only = true;
        }
        else if ("-h" == a || "--help" == a)
        {
            usage(argv[0]);
            return 0;
        }
        else if (file.empty() && !a.empty() && '-' != a.front())
        {
            file = std::string(a);
        }
        else
        {
            usage(argv[0]);
            return 2;
        }
    }
    if (file.empty())
    {
        usage(argv[0]);
        return 2;
    }
    std::string mplane_host;
    int         mplane_port = 0;
    if (mplane_on && !parse_host_port(mplane_addr, &mplane_host, &mplane_port))
    {
        std::fprintf(stderr, "vstreamer: bad --mplane '%s' (want host:port)\n",
                     mplane_addr.c_str());
        return 2;
    }

    vstreamer::app::pipeline_set set;
    std::string                  err;
    if (set.load_file(file, &err) < 0)
    {
        std::fprintf(stderr, "vstreamer: %s\n", err.c_str());
        return 2;
    }
    if (check_only)
    {
        std::printf("%s: ok, %zu nodes\n", file.c_str(), set.nodes().size());
        return 0;
    }

    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    vstreamer::apps::app_console mplane;
    if (mplane_on)
    {
        mplane.set_bind_host(mplane_host.c_str());
        mplane.add_handler(
            [&set](const char *line, std::string &reply) {
                return vstreamer::app::handle_mplane_line(set, line, reply);
            },
            vstreamer::app::mplane_help());
        const int r = mplane.start(mplane_port);
        if (r < 0)
        {
            std::fprintf(stderr, "vstreamer: mplane bind %s failed (%s)\n", mplane_addr.c_str(),
                         std::strerror(-r));
            return 1;
        }
        std::fprintf(stderr, "vstreamer: mplane on udp %s\n", mplane_addr.c_str());
    }

    /* A pipeline that fails to start is retried by the supervisor. */
    const int start_r = set.start("all");
    if (start_r < 0)
    {
        std::fprintf(stderr, "vstreamer: start failed (%s), retrying\n",
                     vstreamer::app::errno_name(start_r));
    }
    set.supervise(g_run, k_restart_backoff);

    mplane.stop();
    (void)set.stop("all");
    return 0;
}
