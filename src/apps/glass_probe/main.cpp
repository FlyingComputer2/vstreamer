/*
 * glass_probe: camera-to-decode latency of an RTP H.264 multicast stream, headless.
 *
 * Joins the multicast group, feeds every datagram to rtp_h264_depay and its output to
 * h264_decoder_mpp, and stamps each access unit twice on the local monotonic clock:
 *   arrival  the depayloader completed the AU (its last RTP packet arrived)
 *   decoded  the decoder handed out the frame
 * Both are measured from the AU's capture instant, which the sender's rtp_h264_pay writes as
 * CLOCK_REALTIME (RTP header extension id 2). The numbers are only as good as the agreement
 * between the sender's CLOCK_REALTIME and this host's; AUs whose capture time is implausible
 * (rtp_h264_depay's skew window) are counted as rejected and left out.
 *
 * Another receiver (the display) can listen to the same group at the same time.
 */
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "core/component_coder.hpp"
#include "core/component_factory.hpp"
#include "core/component_pdu.hpp"
#include "core/sdu_type.hpp"
#include "core/time_util.hpp"

using namespace vstreamer;

namespace
{

volatile std::sig_atomic_t g_running = 1;

void on_signal(int)
{
    g_running = 0;
}

void usage(const char *prog)
{
    std::fprintf(stderr,
                 "Usage: %s [--group 239.255.42.1] [--port 5600] [--interface NAME|IP]\n"
                 "          [--fps 30] [--interval 5] [--frames N]\n"
                 "  --interval S   seconds per report (default 5)\n"
                 "  --frames N     exit after N decoded frames (default: run until signal)\n",
                 prog);
}

struct window
{
    std::vector<double> arrival_ms;
    std::vector<double> decoded_ms;
};

double pct(std::vector<double> v, double p)
{
    if (v.empty())
    {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const size_t i = static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5);
    return v[i];
}

void print_series(const char *name, const std::vector<double> &v)
{
    std::printf(" %s n=%zu min=%.1f p50=%.1f p95=%.1f max=%.1f", name, v.size(), pct(v, 0.0),
                pct(v, 0.5), pct(v, 0.95), pct(v, 1.0));
}

/* Interface by name or IPv4 address; INADDR_ANY when empty. */
bool interface_addr(int fd, const std::string &iface, in_addr *out)
{
    out->s_addr = htonl(INADDR_ANY);
    if (iface.empty())
    {
        return true;
    }
    if (inet_pton(AF_INET, iface.c_str(), out) == 1)
    {
        return true;
    }
    ifreq ifr {};
    std::strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFADDR, &ifr) != 0)
    {
        return false;
    }
    *out = reinterpret_cast<sockaddr_in *>(&ifr.ifr_addr)->sin_addr;
    return true;
}

int open_socket(const std::string &group, int port, const std::string &iface)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return -1;
    }
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    const int rcvbuf = 4 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, group.c_str(), &addr.sin_addr);
    ip_mreq mreq {};
    mreq.imr_multiaddr = addr.sin_addr;
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0 ||
        !interface_addr(fd, iface, &mreq.imr_interface) ||
        setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0)
    {
        std::perror("glass_probe: socket");
        close(fd);
        return -1;
    }
    return fd;
}

std::string query(const component &c, std::string_view key)
{
    std::string v;
    return c.query(key, &v) == 0 ? v : "?";
}

}  // namespace

int main(int argc, char **argv)
{
    std::string group = "239.255.42.1";
    std::string iface;
    std::string fps = "30";
    int         port = 5600;
    int         interval_s = 5;
    long        frames_limit = 0;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h" || i + 1 == argc)
        {
            usage(argv[0]);
            return arg == "--help" || arg == "-h" ? 0 : 2;
        }
        const char *v = argv[++i];
        if (arg == "--group") group = v;
        else if (arg == "--port") port = std::atoi(v);
        else if (arg == "--interface") iface = v;
        else if (arg == "--fps") fps = v;
        else if (arg == "--interval") interval_s = std::max(1, std::atoi(v));
        else if (arg == "--frames") frames_limit = std::atol(v);
        else
        {
            usage(argv[0]);
            return 2;
        }
    }

    auto depay = component_factory::create_coder("rtp_h264_depay");
    auto decode = component_factory::create_coder("h264_decoder_mpp");
    if (!depay || !decode)
    {
        std::fprintf(stderr, "glass_probe: needs rtp_h264_depay and h264_decoder_mpp\n");
        return 1;
    }
    if (depay->configure("fps", fps) < 0 || decode->configure("fps", fps) < 0 ||
        decode->configure("output_size_mode", "stream") < 0 || depay->open() < 0 ||
        decode->open() < 0)
    {
        std::fprintf(stderr, "glass_probe: component setup failed\n");
        return 1;
    }
    const int fd = open_socket(group, port, iface);
    if (fd < 0)
    {
        return 1;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::fprintf(stderr, "glass_probe: %s:%d interface=%s -> depay -> MPP\n", group.c_str(),
                 port, iface.empty() ? "default" : iface.c_str());

    std::vector<uint8_t> buf(65536);
    window               w;
    long                 decoded_total = 0;
    uint64_t             datagrams = 0;
    int64_t              next_report = steady_mono_ns() + int64_t(interval_s) * 1'000'000'000;
    component_pdu        pdu;
    while (g_running)
    {
        pollfd p {fd, POLLIN, 0};
        if (poll(&p, 1, 1) > 0)
        {
            const ssize_t n = recv(fd, buf.data(), buf.size(), 0);
            if (n > 0)
            {
                ++datagrams;
                component_pdu in;
                in.sdu_type = sdu_type_e::RTP;
                in.sdu = bfc::shared_sized_buffer::copy_from(buf.data(), size_t(n));
                // No receive-time fallback: an AU without a plausible capture time keeps
                // ts_us 0 and is left out of the statistics.
                in.ts_us = 0;
                depay->input(std::move(in));
            }
        }
        while (depay->output(pdu) == 0)
        {
            if (!is_caps(pdu.sdu_type) && pdu.ts_us > 0)
            {
                w.arrival_ms.push_back(
                    double(steady_mono_ns() - int64_t(pdu.ts_us) * 1000) / 1e6);
            }
            decode->input(std::move(pdu));
        }
        while (decode->output(pdu) == 0)
        {
            if (!is_caps(pdu.sdu_type) && pdu.ts_us > 0)
            {
                w.decoded_ms.push_back(
                    double(steady_mono_ns() - int64_t(pdu.ts_us) * 1000) / 1e6);
                ++decoded_total;
            }
        }
        const int64_t now = steady_mono_ns();
        if (now >= next_report || (frames_limit > 0 && decoded_total >= frames_limit))
        {
            std::printf("GLASS");
            print_series("arrival_ms", w.arrival_ms);
            print_series("decoded_ms", w.decoded_ms);
            std::printf(" datagrams=%llu skew_ms=%s rejected=%s rtp_lost=%s\n",
                        static_cast<unsigned long long>(datagrams),
                        query(*depay, "capture_skew_ms").c_str(),
                        query(*depay, "capture_ts_rejected").c_str(),
                        query(*depay, "rtp_lost").c_str());
            std::fflush(stdout);
            w = window {};
            next_report = now + int64_t(interval_s) * 1'000'000'000;
            if (frames_limit > 0 && decoded_total >= frames_limit)
            {
                break;
            }
        }
    }
    close(fd);
    decode->close();
    depay->close();
    return 0;
}
