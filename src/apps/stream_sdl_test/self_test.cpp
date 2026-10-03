/* self_test.cpp — stream_sdl_test --self-test. */

#include "apps/stream_sdl_test/self_test.hpp"

#include "apps/stream_sdl_test/diag.hpp"
#include "apps/stream_sdl_test/metrics_sync.hpp"
#include "apps/stream_sdl_test/pipeline_state.hpp"

#include <cinttypes>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include "apps/stream_sdl_test/channel_ports.hpp"

namespace vstreamer::test_app
{

using namespace vstreamer;

int send_channel_console(int console_port, const char *line)
{
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return -errno;
    }
    sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(static_cast<uint16_t>(console_port));
    if (inet_pton(AF_INET, test_app::k_loopback_host, &dst.sin_addr) != 1)
    {
        close(fd);
        return -EINVAL;
    }
    const size_t n = std::strlen(line);
    const ssize_t sent =
        sendto(fd, line, n, 0, reinterpret_cast<sockaddr *>(&dst), sizeof(dst));
    close(fd);
    if (sent != static_cast<ssize_t>(n))
    {
        return sent < 0 ? static_cast<int>(-errno) : -EIO;
    }
    return 0;
}

bool run_self_test(h264_encoder_t &enc, component_sink *preview, stream_receiver &rcv,
                   stream_sender &sender, int console_port,
                   test_app::channel_controller *channel)
{
    std::this_thread::sleep_for(std::chrono::seconds(5));

    std::string stats_val;
    if (preview->query("stats", &stats_val) < 0)
    {
        std::fprintf(stderr, "self-test: display stats query failed\n");
        return false;
    }
    const uint64_t presented = g_bench_diag.rx_present_ok.load();
    const uint64_t nv12 = g_bench_diag.rx_nv12_out.load();
    if (nullptr != std::getenv("DISPLAY") && presented == 0 && nv12 == 0)
    {
        std::fprintf(stderr,
                     "self-test: no decoded/presented frames (sink %.*s diag present_ok=%" PRIu64
                     " nv12_out=%" PRIu64 " rx_udp=%" PRIu64 " au=%" PRIu64 ")\n",
                     static_cast<int>(stats_val.size()), stats_val.data(), presented, nv12,
                     g_bench_diag.rx_udp.load(), g_bench_diag.rx_depay_au.load());
        log_bench_diag(g_bench_diag, rcv, sender, enc, channel);
        return false;
    }

    const int qp_before = query_encoder_qp(enc);
    if (qp_before < 0)
    {
        std::fprintf(stderr, "self-test: could not read encoder qp\n");
        return false;
    }

    if (send_channel_console(console_port, "set_constant_loss 50\n") < 0)
    {
        std::fprintf(stderr, "self-test: channel console command failed\n");
        return false;
    }

    std::this_thread::sleep_for(std::chrono::seconds(5));
    const int qp_after = query_encoder_qp(enc);
    if (qp_after < 0)
    {
        std::fprintf(stderr, "self-test: could not read encoder qp after loss\n");
        return false;
    }

    if (preview->query("stats", &stats_val) < 0)
    {
        std::fprintf(stderr, "self-test: display stats query failed (after loss)\n");
        return false;
    }

    const auto ch = (nullptr != channel) ? channel->forward_stats_snapshot()
                                         : test_app::channel_controller::forward_stats {};

    std::fprintf(stderr,
                 "self-test: qp %d -> %d | ch drop_loss=%" PRIu64 " in=%" PRIu64
                 " | display %.*s | present_ok=%" PRIu64 " nv12_out=%" PRIu64 "\n",
                 qp_before, qp_after, ch.dropped_loss, ch.pkts_in,
                 static_cast<int>(stats_val.size()), stats_val.data(),
                 g_bench_diag.rx_present_ok.load(), g_bench_diag.rx_nv12_out.load());

    if (ch.pkts_in < 16 || ch.dropped_loss < 8)
    {
        std::fprintf(stderr, "self-test: channel did not apply forward loss (check console)\n");
        return false;
    }
    if (qp_after > qp_before)
    {
        return true;
    }
    if (ch.dropped_loss >= 8 && qp_before >= 44)
    {
        std::fprintf(stderr, "self-test: qp at ceiling (%d) with channel loss active\n", qp_before);
        return true;
    }
    std::fprintf(stderr, "self-test: expected qp to rise under 50%% channel loss\n");
    return false;
}

}  // namespace vstreamer::test_app
