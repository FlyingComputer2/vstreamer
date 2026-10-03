#include "apps/common/cpu_map.hpp"
#include "apps/common/pipeline_controller.hpp"
#include "apps/common/queues.hpp"
#include "apps/common/stage_latency.hpp"
#include "apps/common/tx/encoder_types.hpp"
#include "apps/common/tx/source_selector.hpp"
#include "apps/common/tx/source_selector_query_source.hpp"
#include "apps/common/tx/tx_console.hpp"
#include "components/components.hpp"
#include "apps/stream_sdl_test/metrics_sync.hpp"
#include "apps/stream_sdl_test/pipeline_state.hpp"
#include "apps/stream_sdl_test/stages.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace
{

constexpr int k_noise_fallback_w = 640;
constexpr int k_noise_fallback_h = 480;
constexpr int k_noise_fallback_fps = 30;

int cfg(vstreamer::component &c, const char *key, const char *val)
{
    return c.configure(std::string_view(key), std::string_view(val));
}

void usage(const char *prog)
{
    std::fprintf(stderr,
                 "Usage: %s --peer HOST[:PORT] [options]\n"
                 "  --peer HOST[:PORT]   required receiver address\n"
                 "  --device PATH       default /dev/video0\n"
                 "  --size WxH          camera MJPEG (default 1920x1080)\n"
                 "  --fps N             default 30\n"
                 "  --cbr KBPS          default 4000\n"
                 "  --gop N             default fps\n"
                 "  --max-datagram N    default 1476\n"
                 "  --local [HOST:]PORT stream_sender bind (winject static)\n"
                 "  --console [HOST:]PORT default 127.0.0.1:5090\n"
                 "  --mtu N             RTP MTU (default 1400)\n"
                 "  --noise-bandwidth N 0..100 (default 100)\n"
                 "  --noise-block-size N legacy no-op (default 0)\n"
                 "  --noise-pregenerate N default 30 (0=live FFT)\n"
                 "  --no-telemetry      disable reverse telemetry\n"
                 "  --diag\n"
                 "  --help\n",
                 prog);
}

}  // namespace

int main(int argc, char **argv)
{
    using namespace vstreamer;
    using namespace vstreamer::test_app;
    using namespace vstreamer::apps::tx;

    const char *peer = nullptr;
    const char *device = "/dev/video0";
    int         width = 1920;
    int         height = 1080;
    int         fps = 30;
    int         cbr_kbps = 4000;
    int         gop = 0;
    int         max_datagram = 1476;
    const char *local_bind = nullptr;
    std::string console_host = "127.0.0.1";
    int         console_port = 5090;
    int         pregenerate = 30;
    int         mtu = 1400;
    int         noise_bandwidth = 100;
    int         noise_block_size = 0;
    bool        no_telemetry = false;
    bool        diag = false;

    for (int i = 1; i < argc; i++)
    {
        if (0 == std::strcmp(argv[i], "--help") || 0 == std::strcmp(argv[i], "-h"))
        {
            usage(argv[0]);
            return 0;
        }
        if (0 == std::strcmp(argv[i], "--peer") && i + 1 < argc)
        {
            peer = argv[++i];
            continue;
        }
        if (0 == std::strcmp(argv[i], "--device") && i + 1 < argc)
        {
            device = argv[++i];
            continue;
        }
        if (0 == std::strcmp(argv[i], "--size") && i + 1 < argc)
        {
            if (std::sscanf(argv[++i], "%dx%d", &width, &height) != 2)
            {
                std::fprintf(stderr, "bad --size\n");
                return 1;
            }
            continue;
        }
        if (0 == std::strcmp(argv[i], "--fps") && i + 1 < argc)
        {
            fps = std::atoi(argv[++i]);
            continue;
        }
        if (0 == std::strcmp(argv[i], "--cbr") && i + 1 < argc)
        {
            cbr_kbps = std::atoi(argv[++i]);
            continue;
        }
        if (0 == std::strcmp(argv[i], "--gop") && i + 1 < argc)
        {
            gop = std::atoi(argv[++i]);
            continue;
        }
        if (0 == std::strcmp(argv[i], "--max-datagram") && i + 1 < argc)
        {
            max_datagram = std::atoi(argv[++i]);
            continue;
        }
        if (0 == std::strcmp(argv[i], "--local") && i + 1 < argc)
        {
            local_bind = argv[++i];
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
        if (0 == std::strcmp(argv[i], "--mtu") && i + 1 < argc)
        {
            mtu = std::atoi(argv[++i]);
            continue;
        }
        if ((0 == std::strcmp(argv[i], "--noise-bandwidth") ||
             0 == std::strcmp(argv[i], "--noise-randomness")) &&
            i + 1 < argc)
        {
            noise_bandwidth = std::atoi(argv[++i]);
            if (noise_bandwidth < 0 || noise_bandwidth > 100)
            {
                std::fprintf(stderr, "--noise-bandwidth must be 0..100\n");
                return 1;
            }
            continue;
        }
        if (0 == std::strcmp(argv[i], "--noise-block-size") && i + 1 < argc)
        {
            noise_block_size = std::atoi(argv[++i]);
            continue;
        }
        if (0 == std::strcmp(argv[i], "--noise-pregenerate") && i + 1 < argc)
        {
            pregenerate = std::atoi(argv[++i]);
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
    if (nullptr == peer || peer[0] == '\0')
    {
        usage(argv[0]);
        return 1;
    }

    if (const char *cpu_spec = std::getenv("VSTREAMER_CPU_MAP"))
    {
        g_cpu_map = apps::parse_cpu_map(cpu_spec);
    }

    char size_buf[32];
    char fps_buf[16];
    char bw_buf[16];
    char blk_buf[8];
    char pre_buf[16];
    char gop_buf[16];
    char mtu_buf[16];
    char stream_buf[128];
    std::snprintf(size_buf, sizeof(size_buf), "%dx%d", width, height);
    std::snprintf(fps_buf, sizeof(fps_buf), "%d", fps);
    std::snprintf(bw_buf, sizeof(bw_buf), "%d", noise_bandwidth);
    std::snprintf(blk_buf, sizeof(blk_buf), "%d", noise_block_size);
    std::snprintf(pre_buf, sizeof(pre_buf), "%d", pregenerate);
    const int gop_eff = gop > 0 ? gop : fps;
    std::snprintf(gop_buf, sizeof(gop_buf), "%d", gop_eff);
    std::snprintf(mtu_buf, sizeof(mtu_buf), "%d", mtu);
    std::snprintf(stream_buf, sizeof(stream_buf), "%s", peer);

    char noise_size[32];
    std::snprintf(noise_size, sizeof(noise_size), "%dx%d", k_noise_fallback_w, k_noise_fallback_h);
    char noise_fps_buf[16];
    std::snprintf(noise_fps_buf, sizeof(noise_fps_buf), "%d", k_noise_fallback_fps);

    v4l2_source            camera;
    noise_source           noise;
    jpeg_decoder_multicore jdec;
    encoder_t              enc;
    rtp_h264_pay           pay;
    stream_sender          sender;

    if (cfg(camera, "device", device) < 0 || cfg(camera, "size", size_buf) < 0 ||
        cfg(camera, "fps", fps_buf) < 0 || cfg(camera, "format", "mjpeg") < 0 ||
        cfg(noise, "size", noise_size) < 0 || cfg(noise, "fps", noise_fps_buf) < 0 ||
        cfg(noise, "format", "nv12") < 0 || cfg(noise, "noise-bandwidth", bw_buf) < 0 ||
        cfg(noise, "noise-block-size", blk_buf) < 0 ||
        cfg(noise, "pregenerate-frames", pre_buf) < 0)
    {
        return 1;
    }

    cfg(jdec, "size", size_buf);
    cfg(jdec, "fps", fps_buf);
    cfg(jdec, "output_mode", "convert");
    cfg(enc, "size", size_buf);
    cfg(enc, "fps", fps_buf);
    cfg(enc, "gop", gop_buf);
    cfg(pay, "fps", fps_buf);
    cfg(pay, "mtu", mtu_buf);
    cfg(sender, "mtu", mtu_buf);
    cfg(sender, "stream", stream_buf);
    if (nullptr != local_bind)
    {
        cfg(sender, "local", local_bind);
    }
    if (no_telemetry)
    {
        cfg(sender, "telemetry", "off");
    }
    char maxdg[16];
    std::snprintf(maxdg, sizeof(maxdg), "%d", max_datagram);
    cfg(sender, "max_datagram", maxdg);

    char cbr_bps[24];
    std::snprintf(cbr_bps, sizeof(cbr_bps), "%d", cbr_kbps * 1000);
    if (cfg(enc, "cbr", cbr_bps) < 0 || cfg(enc, "rc", "cbr") < 0)
    {
        return 1;
    }

    std::unique_ptr<source_selector>              selector;
    std::unique_ptr<source_selector_query_source> metrics_src;

    const auto on_switch = [&](source_kind kind, int w, int h, int f) {
        char sz[32];
        char fb[16];
        std::snprintf(sz, sizeof(sz), "%dx%d", w, h);
        std::snprintf(fb, sizeof(fb), "%d", f);
        cfg(enc, "size", sz);
        cfg(enc, "fps", fb);
        cfg(pay, "fps", fb);
        cfg(jdec, "size", sz);
        cfg(jdec, "fps", fb);
        (void)enc.configure("idr", std::string_view("1"));
        std::string st = (kind == source_kind::noise_fallback) ? "noise_fallback" : "camera";
        metric_store(*g_pipeline_metrics.get_metric("source.state"), st);
        std::fprintf(stderr, "uvc_stream_sender: source -> %s %s @ %d\n", st.c_str(), sz, f);
    };

    selector = std::make_unique<source_selector>(
        camera, noise, k_noise_fallback_w, k_noise_fallback_h, k_noise_fallback_fps, on_switch,
        source_selector::push_packet_fn {}, source_selector::push_packet_fn {});
    metrics_src = std::make_unique<source_selector_query_source>(*selector);
    g_tx.metrics_source = metrics_src.get();

    if (selector->open() < 0 || jdec.open() < 0 || enc.open() < 0 || sender.open() < 0 ||
        pay.open() < 0)
    {
        return 1;
    }
    sender.set_enabled(true, 0);

    g_diag = diag;
    apps::stage_latency_set_diag_enabled(diag);
    g_tx.stream_fps.store(fps);

    const size_t pipe_q = apps::queue_depth_from_env("VSTREAMER_PIPE_QUEUE_DEPTH",
                                                     apps::k_default_pipe_queue_depth, 64);
    apps::pipeline_queue mjpeg_q(pipe_q, g_run);
    apps::pipeline_queue nv12_q(pipe_q, g_run);
    g_tx.metrics_nv12_q = &nv12_q;

    selector->set_push_handlers(
        [&](data_packet &&p) {
            (void)enqueue_source_frame(std::move(p), &mjpeg_q, &nv12_q, &g_bench_diag);
        },
        [&](data_packet &&p) {
            (void)enqueue_source_frame(std::move(p), &mjpeg_q, &nv12_q, &g_bench_diag);
        });

    pipeline_rate_state       rate;
    apps::pipeline_controller ctrl;
    ctrl.bind_legacy_run(&g_run);
    ctrl.set_diag_enabled(diag);
    apps::app_console &console = ctrl.console();
    console.set_bind_host(console_host.c_str());
    console.set_pipeline_metrics(&g_pipeline_metrics);
    console.set_pipeline_metrics_sync_live([&]() {
        sync_pipeline_metrics_live(g_bench_diag, &sender, &enc, nullptr, nullptr);
    });
    console.set_source_state_metrics_refresh([&]() {
        std::string st = "running";
        if (nullptr != g_tx.metrics_source)
        {
            (void)query_source_metric_string(g_tx.metrics_source, "state", st);
        }
        metric_store(*g_pipeline_metrics.get_metric("source.state"), st);
    });
    apps::tx::tx_console_targets tx_targets;
    tx_targets.sender = &sender;
    tx_targets.encoder = &enc;
    tx_targets.set_cbr_kbps = [&](int kbps) {
        g_tx.pending_console_cbr_kbps.store(kbps, std::memory_order_release);
        return true;
    };
    tx_targets.set_qp = [&](int qp) {
        g_tx.pending_console_qp.store(qp, std::memory_order_release);
        return true;
    };
    tx_targets.set_gop = [&](int g) {
        g_tx.pending_console_gop.store(g, std::memory_order_release);
        return true;
    };
    tx_targets.force_idr = [&]() {
        g_tx.pending_console_idr.store(true, std::memory_order_release);
        return true;
    };
    apps::tx::register_tx_console_handlers(console, tx_targets);
    if (console.start(console_port) < 0)
    {
        return 1;
    }

    const int jpeg_workers = static_cast<int>(g_cpu_map.jpeg_workers.size());
    ctrl.add_stage("source", "source",
                   [&, sel = selector.get()](std::atomic<bool> & /*run*/) {
                       source_stage_selector_main(sel, &mjpeg_q, &nv12_q, &g_bench_diag);
                   });
    ctrl.add_stage("jpeg", "jpeg", [&, jw = jpeg_workers](std::atomic<bool> & /*run*/) {
        jpeg_stage_main(&jdec, &mjpeg_q, &nv12_q, &g_bench_diag, jw);
    });
    ctrl.add_stage("encode", "encode", [&](std::atomic<bool> & /*run*/) {
        encode_stage_main(&enc, &pay, &sender, &nv12_q, &g_bench_diag);
    });
    ctrl.add_metrics_sync([&]() {
        update_pipeline_metrics(g_bench_diag, &enc, &sender, nullptr, nullptr, false, rate,
                                nullptr, &jdec, true, nullptr);
    });

    std::fprintf(stderr, "uvc_stream_sender: peer %s device %s %s @ %d console %s:%d\n", peer,
                 device, size_buf, fps, console_host.c_str(), console_port);
    ctrl.run();

    mjpeg_q.wake_shutdown();
    nv12_q.wake_shutdown();
    console.stop();
    selector->close();
    sender.close();
    enc.close();
    jdec.close();
    pay.close();
    return 0;
}
