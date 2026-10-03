#include "apps/common/app_console.hpp"
#include "apps/common/pipeline_controller.hpp"
#include "apps/common/cpu_map.hpp"
#include "apps/common/queues.hpp"
#include "apps/common/stage_latency.hpp"
#include "components/components.hpp"
#include "apps/stream_sdl_test/metrics_sync.hpp"
#include "apps/stream_sdl_test/pipeline_state.hpp"
#include "apps/stream_sdl_test/stages.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace
{

int cfg(vstreamer::component &c, const char *key, const char *val)
{
    return c.configure(std::string_view(key), std::string_view(val));
}

void usage(const char *prog)
{
    std::fprintf(stderr,
                 "Usage: %s [options]\n"
                 "  --listen [HOST:]PORT  default 0.0.0.0:5001\n"
                 "  --display sdl|kmsdrm  default sdl\n"
                 "  --max-datagram N      default 1476\n"
                 "  --console [HOST:]PORT default 127.0.0.1:5091\n"
                 "  --no-telemetry\n"
                 "  --diag\n"
                 "  --help\n",
                 prog);
}

}  // namespace

int main(int argc, char **argv)
{
    using namespace vstreamer;
    using namespace vstreamer::test_app;

    const char *listen = "0.0.0.0:5001";
    const char *display_mode = "sdl";
    int         max_datagram = 1476;
    std::string console_host = "127.0.0.1";
    int         console_port = 5091;
    bool        no_telemetry = false;
    bool        diag = false;
    int         width = 1920;
    int         height = 1080;
    int         fps = 30;

    for (int i = 1; i < argc; i++)
    {
        if (0 == std::strcmp(argv[i], "--help") || 0 == std::strcmp(argv[i], "-h"))
        {
            usage(argv[0]);
            return 0;
        }
        if (0 == std::strcmp(argv[i], "--listen") && i + 1 < argc)
        {
            listen = argv[++i];
            continue;
        }
        if (0 == std::strcmp(argv[i], "--display") && i + 1 < argc)
        {
            display_mode = argv[++i];
            continue;
        }
        if (0 == std::strcmp(argv[i], "--max-datagram") && i + 1 < argc)
        {
            max_datagram = std::atoi(argv[++i]);
            continue;
        }
        if (0 == std::strcmp(argv[i], "--console") && i + 1 < argc)
        {
            const char *spec = argv[++i];
            const char *colon = std::strchr(spec, ':');
            if (nullptr != colon)
            {
                console_host.assign(spec, colon - spec);
                console_port = std::atoi(colon + 1);
            }
            else
            {
                console_port = std::atoi(spec);
            }
            continue;
        }
        if (0 == std::strcmp(argv[i], "--no-telemetry"))
        {
            no_telemetry = true;
            continue;
        }
        if (0 == std::strcmp(argv[i], "--diag"))
        {
            diag = true;
            continue;
        }
        std::fprintf(stderr, "unknown option %s\n", argv[i]);
        return 1;
    }

    if (const char *cpu_spec = std::getenv("VSTREAMER_CPU_MAP"))
    {
        g_cpu_map = apps::parse_cpu_map(cpu_spec);
    }

    const bool kmsdrm = (0 == std::strcmp(display_mode, "kmsdrm"));
    stream_receiver rcv;
    rtp_h264_depay depay;
    h264_decoder_mpp dec;
    sdl_sink         display;
    component_sink  *preview = &display;

    char maxdg[16];
    std::snprintf(maxdg, sizeof(maxdg), "%d", max_datagram);
    if (cfg(rcv, "listen", listen) < 0 || cfg(rcv, "max_datagram", maxdg) < 0)
    {
        return 1;
    }
    if (no_telemetry)
    {
        cfg(rcv, "telemetry", "off");
    }
    char size_buf[32];
    char fps_buf[16];
    std::snprintf(size_buf, sizeof(size_buf), "%dx%d", width, height);
    std::snprintf(fps_buf, sizeof(fps_buf), "%d", fps);
    cfg(dec, "size", size_buf);
    cfg(dec, "fps", fps_buf);
    cfg(depay, "fps", fps_buf);
    cfg(*preview, "title", "sdl_stream_receiver");
    if (kmsdrm && cfg(*preview, "video_driver", "kmsdrm") < 0)
    {
        return 1;
    }

    const bool defer_sdl = !kmsdrm;
    if (rcv.open() < 0 || depay.open() < 0 || (defer_sdl ? 0 : preview->open()) < 0)
    {
        return 1;
    }

    g_diag = diag;
    apps::stage_latency_set_diag_enabled(diag);
    g_tx.stream_fps.store(fps);
    if (nullptr != std::getenv("VSTREAMER_SKIP_DECODE"))
    {
        g_rx.skip_decode = true;
        std::fprintf(stderr, "sdl_stream_receiver: decode disabled (VSTREAMER_SKIP_DECODE)\n");
    }

    const size_t present_q_depth =
        apps::queue_depth_from_env("VSTREAMER_PRESENT_QUEUE_DEPTH",
                                   apps::k_default_present_queue_depth, 16);
    const size_t rx_au_q_depth =
        apps::queue_depth_from_env("VSTREAMER_RX_AU_QUEUE_DEPTH",
                                   apps::k_default_rx_au_queue_depth, 256);
    apps::present_frame_queue present_q(present_q_depth, g_run);
    apps::rx_au_queue         au_q(rx_au_q_depth, g_run);

    pipeline_rate_state       rate;
    apps::pipeline_controller ctrl;
    ctrl.bind_legacy_run(&g_run);
    ctrl.set_diag_enabled(diag);
    apps::app_console &console = ctrl.console();
    console.set_bind_host(console_host.c_str());
    console.set_pipeline_metrics(&g_pipeline_metrics);
    console.set_pipeline_metrics_sync_live([&]() {
        sync_pipeline_metrics_live(g_bench_diag, nullptr, nullptr, &rcv, nullptr);
    });
    if (console.start(console_port) < 0)
    {
        return 1;
    }

    ctrl.add_stage("rx_net", "rx",
                   [&](std::atomic<bool> & /*run*/) {
                       rx_net_thread_main(&rcv, &depay, &au_q, &g_bench_diag);
                   });
    ctrl.add_stage("decode", "rx",
                   [&](std::atomic<bool> & /*run*/) {
                       decode_thread_main(&dec, &present_q, &au_q, &g_bench_diag);
                   });
    ctrl.add_stage("present", "",
                   [&](std::atomic<bool> & /*run*/) {
                       present_thread_main(preview, &present_q, width, height, kmsdrm, defer_sdl,
                                           &g_bench_diag);
                   });
    ctrl.add_stage("telemetry", "",
                   [&](std::atomic<bool> & /*run*/) {
                       apps::rx::telemetry_thread_main(&rcv);
                   });
    ctrl.add_metrics_sync([&]() {
        update_pipeline_metrics(g_bench_diag, nullptr, nullptr, &rcv, preview, kmsdrm, rate,
                                nullptr, nullptr, false, &dec);
    });

    std::fprintf(stderr, "sdl_stream_receiver: listen %s console %s:%d\n", listen,
                 console_host.c_str(), console_port);
    ctrl.run();

    rcv.close();
    au_q.wake();
    present_q.wake();
    console.stop();
    depay.close();
    dec.close();
    if (!defer_sdl)
    {
        preview->close();
    }
    return 0;
}
