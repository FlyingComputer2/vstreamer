/*
 * Bench: synthetic noise through RTP loopback + impaired UDP channel → SDL.
 *
 * Pipeline threads (default CPU affinity 0–3 on Linux):
 *   CPU0: noise_source (NV12) or v4l2_source (--source) → queue
 *   CPU1: jpeg_decoder_multicore (v4l2 MJPEG only) → queue
 *   CPU2: h264_encoder_mpp → rtp_h264_pay → stream_sender
 *   CPU3: stream_receiver → rtp_h264_depay → h264_decoder_mpp → display sink
 * Rate: encoder CBR/QP at open; adjust via UDP set_encode_cbr / set_encode_qp.
 * Link: stream_sender → channel_controller (fwd) → stream_receiver
 *       stream_receiver link reports → channel_controller (rev = return path of the fwd flow,
 *       NAT-style) → stream_sender; peer_* metrics come from the sender's received reports.
 *
 * Default UDP ports (channel_ports.hpp): fwd 5000→5001, console 5090.
 *
 * channel_controller console (UDP, newline-terminated):
 *   help | h | ?
 *   set_max_kbps <kbps>
 *   set_constant_loss <pct>
 *   set_fec none | set_fec_k <k> | set_fec_n <n> | set_fec_spread <ms> | set_fec_timeout <ms>
 *   set_encode_cbr <kbps> | set_encode_qp <qp> | set_gop <gop> | force_idr | get_metric <name>
 *   ping / stats / metrics | get | empty line → pipeline metrics report
 *
 * Low-latency queue defaults (override with env):
 *   VSTREAMER_PIPE_QUEUE_DEPTH (default 8, see k_default_pipe_queue_depth)
 *   VSTREAMER_PRESENT_QUEUE_DEPTH (default 1)
 *   VSTREAMER_RX_AU_QUEUE_DEPTH (default 4)
 *   VSTREAMER_CPU_MAP (stage=cpu lists; see docs/vstreamer.md)
 *
 * CLI: --chan-bind ADDR (channel UDP console + relay ingress bind; default 127.0.0.1)
 * UVC + kmsdrm (former uvc_jpegdec_kmsdrm target): stream_sdl --display kmsdrm --source /dev/video0
 * Metric latency.* / h264_encoder.latency_ms / h264_decoder.latency_ms /
 * sdl_sink.latency_ms / stream_sdl.glass_latency_ms (capture → present, including across
 * hosts via CLOCK_REALTIME on the wire; sender and receiver clocks must be synchronized).
 *
 * Per-stage latency lines (stderr): --diag or VSTREAMER_LOG_STAGE_LATENCY=1
 * Optional: VSTREAMER_STAGE_LATENCY_EVERY=N (log every Nth frame by pts, default 1).
 */
#include "components/components.hpp"

#if defined(ENABLE_V4L2_SOURCE) && defined(ENABLE_NOISE_SOURCE)
#include "apps/common/tx/source_selector.hpp"
#include "apps/common/tx/source_selector_query_source.hpp"
#endif
#include "apps/stream_sdl_test/channel_controller.hpp"
#include "apps/stream_sdl_test/channel_ports.hpp"
#include "apps/common/cpu_map.hpp"
#include "apps/common/pipeline_controller.hpp"
#include "apps/common/queues.hpp"
#include "apps/common/stage_latency.hpp"
#include "apps/stream_sdl_test/diag.hpp"
#include "apps/stream_sdl_test/encoder_types.hpp"
#include "apps/stream_sdl_test/metrics_sync.hpp"
#include "apps/stream_sdl_test/pipeline_state.hpp"
#include "apps/stream_sdl_test/self_test.hpp"
#include "apps/stream_sdl_test/stages.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <atomic>
#include <memory>
#include <chrono>
#include <csignal>
#include <string>
#include <thread>

#include <unistd.h>

#include "core/component_source.hpp"
#include "core/metrics.hpp"

namespace vstreamer::test_app
{

using namespace vstreamer;

void on_signal(int /*sig*/)
{
    g_run = false;
}

void shutdown_pipeline(h264_encoder_t &enc, h264_decoder_mpp &dec, stream_sender &sender,
                       stream_receiver &rcv, jpeg_decoder_multicore *jdec, v4l2_source *v4l2,
                       apps::pipeline_queue *mjpeg_q, apps::pipeline_queue *nv12_q)
{
    g_run = false;
#if defined(ENABLE_H264_ENCODER_MPP)
    enc.cancel_pending_io();
#endif
    dec.cancel_pending_io();
    sender.close();
    rcv.close();
#ifdef ENABLE_V4L2_SOURCE
    if (nullptr != v4l2)
    {
        v4l2->interrupt_shutdown();
    }
#endif
    if (nullptr != jdec)
    {
        jdec->close();
    }
    if (nullptr != mjpeg_q)
    {
        mjpeg_q->wake_shutdown();
    }
    if (nullptr != nv12_q)
    {
        nv12_q->wake_shutdown();
    }
}
void print_usage(const char *prog)
{
    using test_app::k_chan_console;
    using test_app::k_chan_fwd_ingress;
    using test_app::k_loopback_host;
    using test_app::k_stream_rx_listen;

    std::fprintf(stderr,
                 "Usage: %s [options]\n"
                 "  --size WxH     default 416x240 (rover); noise is always NV12 snow\n"
                 "  --fps N        default 30\n"
                 "  --noise-bandwidth N    0..100 shaped spectrum→SIMD IFFT (0=flat; default 100)\n"
                 "  --noise-block-size N   legacy no-op (default 0)\n"
                 "  --pregenerate-frame N  prebuild N NV12 frames and loop (0=live; max 128)\n"
                 "  (env VSTREAMER_NOISE_THREADS=1..64 OpenMP threads for noise IFFT; default all)\n"
                 "  (env VSTREAMER_PREGENERATE_FRAMES same as --pregenerate-frame)\n"
                 "  --chan-bind A  channel console + relay bind address (default %s)\n"
                 "  --chan-in P    channel forward ingress (default %d)\n"
                 "  --channel-drop-dt-ms MS  max_kbps window (default %d)\n"
                 "  --channel-queue N  per-direction ingress queue (0=off; default %d)\n"
                 "  --rx-port P    stream_receiver listen (default %d)\n"
                 "  --console P    channel UDP console + metrics (default %d)\n"
                 "  --display MODE sdl (default) or kmsdrm (SDL kmsdrm / DRM)\n"
                 "  (UVC+kmsdrm: --display kmsdrm --source /dev/video0)\n"
                 "  --self-test    run ~8s, verify display frames and adaptive qp\n"
                 "  --diag         pipeline counters + per-stage latency (stderr)\n"
                 "                 (or VSTREAMER_LOG_STAGE_LATENCY=1; EVERY=N th frame)\n"
                 "  --cbr KBPS     encoder CBR target kb/s (default %d)\n"
                 "  --gop N        encoder GOP 1..255 (default: same as --fps; or VSTREAMER_ENC_GOP)\n"
                 "  --source ARG   noise (default) or V4L2 device e.g. /dev/video0\n"
                 "  --no-telemetry stream_receiver/stream_sender link reports off\n"
                 "                 (peer_* metrics stay 0, peer_report_age_ms -1)\n",
                 prog, k_loopback_host, k_chan_fwd_ingress,
                 test_app::k_chan_default_drop_dt_ms,
                 test_app::k_chan_default_queue_depth, k_stream_rx_listen,
                 k_chan_console,
                 test_app::k_encoder_default_cbr_kbps);
}

[[nodiscard]] bool source_arg_is_noise(const char *arg)
{
    return nullptr == arg || arg[0] == '\0' || 0 == std::strcmp(arg, "noise") ||
           0 == std::strcmp(arg, "snow");
}

[[nodiscard]] bool source_arg_is_v4l2_device(const char *arg)
{
    return nullptr != arg && arg[0] != '\0' && !source_arg_is_noise(arg) &&
           std::strncmp(arg, "/dev/", 5) == 0;
}

[[nodiscard]] bool display_use_kmsdrm(const char *mode)
{
    return 0 == std::strcmp(mode, "kmsdrm") || 0 == std::strcmp(mode, "sdl_kmsdrm") ||
           0 == std::strcmp(mode, "sdl_kmsdrm_sink");
}

}  // namespace vstreamer::test_app

int main(int argc, char **argv)
{
    using namespace vstreamer;
    using namespace vstreamer::test_app;
    using test_app::k_chan_console;
    using test_app::k_chan_fwd_ingress;
    using test_app::k_loopback_host;
    using test_app::k_stream_rx_listen;

    int width = 416;
    int height = 240;
    int fps = 30;
    int noise_bandwidth = 100;
    int noise_block_size = 0;
    int pregenerate_frames = 0;
    if (const char *bw_env = std::getenv("VSTREAMER_NOISE_BANDWIDTH");
        nullptr != bw_env && bw_env[0] != '\0')
    {
        noise_bandwidth = std::atoi(bw_env);
        if (noise_bandwidth < 0 || noise_bandwidth > 100)
        {
            std::fprintf(stderr, "VSTREAMER_NOISE_BANDWIDTH must be 0..100\n");
            return 1;
        }
    }
    else if (const char *rnd_env = std::getenv("VSTREAMER_NOISE_RANDOMNESS");
             nullptr != rnd_env && rnd_env[0] != '\0')
    {
        noise_bandwidth = std::atoi(rnd_env);
        if (noise_bandwidth < 0 || noise_bandwidth > 100)
        {
            std::fprintf(stderr, "VSTREAMER_NOISE_RANDOMNESS must be 0..100 (deprecated; use "
                                 "VSTREAMER_NOISE_BANDWIDTH)\n");
            return 1;
        }
    }
    if (const char *blk_env = std::getenv("VSTREAMER_NOISE_BLOCK_SIZE");
        nullptr != blk_env && blk_env[0] != '\0')
    {
        noise_block_size = std::atoi(blk_env);
        if (noise_block_size < 0 || noise_block_size > 256)
        {
            std::fprintf(stderr, "VSTREAMER_NOISE_BLOCK_SIZE must be 0..256 (0=auto)\n");
            return 1;
        }
    }
    if (const char *preg_env = std::getenv("VSTREAMER_PREGENERATE_FRAMES");
        nullptr != preg_env && preg_env[0] != '\0')
    {
        pregenerate_frames = std::atoi(preg_env);
        if (pregenerate_frames < 0 || pregenerate_frames > 128)
        {
            std::fprintf(stderr, "VSTREAMER_PREGENERATE_FRAMES must be 0..128\n");
            return 1;
        }
    }
    int chan_in = k_chan_fwd_ingress;
    int channel_drop_dt_ms = test_app::k_chan_default_drop_dt_ms;
    int channel_queue = test_app::k_chan_default_queue_depth;
    int rx_port = k_stream_rx_listen;
    int console_port = k_chan_console;
    const char *chan_bind_host = k_loopback_host;
    const char *display_mode = "sdl";
#ifdef ENABLE_NOISE_SOURCE
    const char *source_arg = "noise";
#else
    const char *source_arg = "/dev/video0";
#endif
    const char *app_label = "stream_sdl";
    bool self_test = false;
    bool diag_log = false;
    bool telemetry = true;
    int  encoder_cbr_kbps = test_app::k_encoder_default_cbr_kbps;
    int  encoder_gop = 0;

    for (int i = 1; i < argc; i++)
    {
        if (0 == std::strcmp(argv[i], "--size") && i + 1 < argc)
        {
            if (std::sscanf(argv[++i], "%dx%d", &width, &height) != 2)
            {
                print_usage(argv[0]);
                return 1;
            }
        }
        else if (0 == std::strcmp(argv[i], "--fps") && i + 1 < argc)
        {
            fps = std::atoi(argv[++i]);
        }
        else if ((0 == std::strcmp(argv[i], "--noise-bandwidth") ||
                  0 == std::strcmp(argv[i], "--noise-randomness")) &&
                 i + 1 < argc)
        {
            noise_bandwidth = std::atoi(argv[++i]);
            if (noise_bandwidth < 0 || noise_bandwidth > 100)
            {
                std::fprintf(stderr, "--noise-bandwidth must be 0..100\n");
                return 1;
            }
        }
        else if (0 == std::strcmp(argv[i], "--noise-block-size") && i + 1 < argc)
        {
            noise_block_size = std::atoi(argv[++i]);
            if (noise_block_size < 0 || noise_block_size > 256)
            {
                std::fprintf(stderr, "--noise-block-size must be 0..256 (0=auto)\n");
                return 1;
            }
        }
        else if ((0 == std::strcmp(argv[i], "--pregenerate-frame") ||
                  0 == std::strcmp(argv[i], "--pregenerate-frames")) &&
                 i + 1 < argc)
        {
            pregenerate_frames = std::atoi(argv[++i]);
            if (pregenerate_frames < 0 || pregenerate_frames > 128)
            {
                std::fprintf(stderr, "--pregenerate-frame must be 0..128 (0=live IFFT each frame)\n");
                return 1;
            }
        }
        else if (0 == std::strcmp(argv[i], "--chan-bind") && i + 1 < argc)
        {
            chan_bind_host = argv[++i];
        }
        else if (0 == std::strcmp(argv[i], "--chan-in") && i + 1 < argc)
        {
            chan_in = std::atoi(argv[++i]);
        }
        else if (0 == std::strcmp(argv[i], "--channel-drop-dt-ms") && i + 1 < argc)
        {
            channel_drop_dt_ms = std::atoi(argv[++i]);
            if (channel_drop_dt_ms < test_app::k_chan_min_drop_dt_ms ||
                channel_drop_dt_ms > test_app::k_chan_max_drop_dt_ms)
            {
                std::fprintf(stderr, "--channel-drop-dt-ms must be %d..%d\n",
                             test_app::k_chan_min_drop_dt_ms, test_app::k_chan_max_drop_dt_ms);
                return 1;
            }
        }
        else if (0 == std::strcmp(argv[i], "--channel-queue") && i + 1 < argc)
        {
            channel_queue = std::atoi(argv[++i]);
            if (channel_queue < 0 || channel_queue > 65535)
            {
                std::fprintf(stderr, "--channel-queue must be 0..65535\n");
                return 1;
            }
        }
        else if (0 == std::strcmp(argv[i], "--rx-port") && i + 1 < argc)
        {
            rx_port = std::atoi(argv[++i]);
        }
        else if (0 == std::strcmp(argv[i], "--console") && i + 1 < argc)
        {
            console_port = std::atoi(argv[++i]);
        }
        else if (0 == std::strcmp(argv[i], "--display") && i + 1 < argc)
        {
            display_mode = argv[++i];
            if (!display_use_kmsdrm(display_mode) && 0 != std::strcmp(display_mode, "sdl") &&
                0 != std::strcmp(display_mode, "default") && 0 != std::strcmp(display_mode, "display"))
            {
                std::fprintf(stderr, "unknown --display mode: %s\n", display_mode);
                print_usage(argv[0]);
                return 1;
            }
        }
        else if (0 == std::strcmp(argv[i], "--self-test"))
        {
            self_test = true;
            diag_log = true;
        }
        else if (0 == std::strcmp(argv[i], "--no-telemetry"))
        {
            telemetry = false;
        }
        else if (0 == std::strcmp(argv[i], "--diag"))
        {
            diag_log = true;
        }
        else if (0 == std::strcmp(argv[i], "--cbr") && i + 1 < argc)
        {
            encoder_cbr_kbps = std::atoi(argv[++i]);
            if (encoder_cbr_kbps < 100 || encoder_cbr_kbps > 200'000)
            {
                std::fprintf(stderr, "--cbr must be 100..200000 kb/s\n");
                return 1;
            }
        }
        else if (0 == std::strcmp(argv[i], "--gop") && i + 1 < argc)
        {
            encoder_gop = std::atoi(argv[++i]);
            if (encoder_gop < 1 || encoder_gop > 255)
            {
                std::fprintf(stderr, "--gop must be 1..255\n");
                return 1;
            }
        }
        else if (0 == std::strcmp(argv[i], "--source") && i + 1 < argc)
        {
            source_arg = argv[++i];
            if (!source_arg_is_noise(source_arg) && !source_arg_is_v4l2_device(source_arg))
            {
                std::fprintf(stderr,
                             "unknown --source %s (use noise or a path like /dev/video0)\n",
                             source_arg);
                print_usage(argv[0]);
                return 1;
            }
#ifndef ENABLE_V4L2_SOURCE
            if (!source_arg_is_noise(source_arg))
            {
                std::fprintf(stderr,
                             "stream_sdl: V4L2 capture not built (ENABLE_V4L2_SOURCE=OFF); "
                             "use --source noise\n");
                return 1;
            }
#endif
        }
        else if (0 == std::strcmp(argv[i], "--help"))
        {
            print_usage(argv[0]);
            return 0;
        }
        else
        {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (height < 2 || (height % 2) != 0 || width < 2 || (width % 2) != 0)
    {
        std::fprintf(stderr, "size width and height must be even (min 2)\n");
        return 1;
    }

    if (const char *cpu_spec = std::getenv("VSTREAMER_CPU_MAP"))
    {
        g_cpu_map = apps::parse_cpu_map(cpu_spec);
    }
    else
    {
        g_cpu_map = apps::parse_cpu_map("");
    }
#if defined(ENABLE_H264_ENCODER_CEDAR)
    if (width < 32 || (width % 32) != 0)
    {
        std::fprintf(stderr, "cedar encoder requires width aligned to 32\n");
        return 1;
    }
#endif
    if (encoder_gop == 0)
    {
        if (const char *gop_env = std::getenv("VSTREAMER_ENC_GOP");
            nullptr != gop_env && gop_env[0] != '\0')
        {
            encoder_gop = std::atoi(gop_env);
            if (encoder_gop < 1 || encoder_gop > 255)
            {
                std::fprintf(stderr, "VSTREAMER_ENC_GOP must be 1..255\n");
                return 1;
            }
        }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    char size_buf[32];
    char fps_buf[16];
    char bw_buf[8];
    char blk_buf[8];
    char pregen_buf[8];
    char qp_buf[8];
    char gop_buf[8];
    char mtu_buf[8];
    std::snprintf(size_buf, sizeof(size_buf), "%dx%d", width, height);
    std::snprintf(fps_buf, sizeof(fps_buf), "%d", fps);
    std::snprintf(bw_buf, sizeof(bw_buf), "%d", noise_bandwidth);
    std::snprintf(blk_buf, sizeof(blk_buf), "%d", noise_block_size);
    std::snprintf(pregen_buf, sizeof(pregen_buf), "%d", pregenerate_frames);
    int default_qp = (width * height >= 1920 * 1080) ? 42 : 36;
    if (const char *qp_env = std::getenv("VSTREAMER_ENC_QP"); nullptr != qp_env && qp_env[0] != '\0')
    {
        default_qp = std::atoi(qp_env);
    }
    std::snprintf(qp_buf, sizeof(qp_buf), "%d", default_qp);
    const int gop_effective = encoder_gop > 0 ? encoder_gop : fps;
    std::snprintf(gop_buf, sizeof(gop_buf), "%d", gop_effective);
    std::snprintf(mtu_buf, sizeof(mtu_buf), "%d", 1400);

    char stream_buf[64];
    char listen_buf[64];
    std::snprintf(stream_buf, sizeof(stream_buf), "%s:%d", k_loopback_host, chan_in);
    std::snprintf(listen_buf, sizeof(listen_buf), "%s:%d", k_loopback_host, rx_port);

    const bool use_v4l2 = source_arg_is_v4l2_device(source_arg);

#ifdef ENABLE_NOISE_SOURCE
    noise_source noise;
#endif
#ifdef ENABLE_V4L2_SOURCE
    v4l2_source camera;
#endif
#if defined(ENABLE_V4L2_SOURCE) && defined(ENABLE_NOISE_SOURCE)
    std::unique_ptr<apps::tx::source_selector>              uvc_selector;
    std::unique_ptr<apps::tx::source_selector_query_source> uvc_metrics_source;
#endif
    jpeg_decoder_multicore jdec;
    h264_encoder_t enc;
    rtp_h264_pay pay;
    stream_sender sender;
    stream_receiver rcv;
    rtp_h264_depay depay;
    h264_decoder_mpp dec;
    sdl_sink display;
    component_sink *preview = &display;
    const bool kmsdrm = display_use_kmsdrm(display_mode);
    if (kmsdrm && cfg_str(display, "video_driver", "kmsdrm") < 0)
    {
        return 1;
    }
    test_app::channel_controller channel;

    const bool defer_sdl_to_present = !kmsdrm;

    component_source *source = nullptr;
    const char *source_open_label = "noise_source";
    bool         use_uvc_selector = false;
    if (use_v4l2)
    {
#ifdef ENABLE_V4L2_SOURCE
#if defined(ENABLE_NOISE_SOURCE)
        use_uvc_selector = true;
        source_open_label = "source_selector";
#else
        source = &camera;
        source_open_label = "v4l2_source";
#endif
        if (cfg_str(camera, "device", source_arg) < 0 || cfg_str(camera, "size", size_buf) < 0 ||
            cfg_str(camera, "fps", fps_buf) < 0 || cfg_str(camera, "format", "mjpeg") < 0)
        {
            return 1;
        }
#if defined(ENABLE_NOISE_SOURCE)
        if (cfg_str(noise, "size", size_buf) < 0 || cfg_str(noise, "fps", fps_buf) < 0 ||
            cfg_str(noise, "format", "nv12") < 0 || cfg_str(noise, "noise-bandwidth", bw_buf) < 0 ||
            cfg_str(noise, "noise-block-size", blk_buf) < 0 ||
            cfg_str(noise, "pregenerate-frames", pregen_buf) < 0)
        {
            return 1;
        }
#endif
#else
        std::fprintf(stderr, "stream_sdl: V4L2 not available\n");
        return 1;
#endif
    }
    else
    {
#ifdef ENABLE_NOISE_SOURCE
        source = &noise;
        if (cfg_str(noise, "size", size_buf) < 0 || cfg_str(noise, "fps", fps_buf) < 0 ||
            cfg_str(noise, "noise-bandwidth", bw_buf) < 0 ||
            cfg_str(noise, "noise-block-size", blk_buf) < 0 ||
            cfg_str(noise, "pregenerate-frames", pregen_buf) < 0)
        {
            return 1;
        }
#else
        std::fprintf(stderr, "stream_sdl: noise source not available; use --source /dev/video0\n");
        return 1;
#endif
    }
    const bool use_jpeg_decode = use_v4l2;
    cfg_str(jdec, "size", size_buf);
    cfg_str(jdec, "fps", fps_buf);
    {
        char workers_buf[16];
        std::snprintf(workers_buf, sizeof(workers_buf), "%zu", g_cpu_map.jpeg_workers.size());
        const std::string jw_list = apps::format_cpulist(g_cpu_map.jpeg_workers);
        cfg_str(jdec, "workers", workers_buf);
        cfg_str(jdec, "worker_cpus", jw_list.c_str());
    }
    if (use_v4l2)
    {
        cfg_str(jdec, "output_mode", "convert");
    }
    cfg_str(enc, "size", size_buf);
    cfg_str(enc, "fps", fps_buf);
    cfg_str(enc, "qp", qp_buf);
    cfg_str(enc, "gop", gop_buf);
    cfg_str(pay, "fps", fps_buf);
    cfg_str(depay, "fps", fps_buf);
    cfg_str(pay, "mtu", mtu_buf);
    cfg_str(sender, "stream", stream_buf);
    cfg_str(sender, "mtu", mtu_buf);
    /* Winject WiFi MPDU cap (1476-byte UDP payload including the stream header). */
    cfg_str(sender, "max_datagram", "1476");
    cfg_str(rcv, "max_datagram", "1476");
    if (const char *pace = std::getenv("VSTREAMER_WIRE_PACE_KBPS");
        nullptr != pace && pace[0] != '\0' && 0 != std::strcmp(pace, "0"))
    {
        if (cfg_str(sender, "max_kbps", pace) < 0)
        {
            return 1;
        }
        std::fprintf(stderr, "%s: UDP wire pace %s kb/s (VSTREAMER_WIRE_PACE_KBPS)\n", app_label,
                     pace);
    }
    cfg_str(rcv, "listen", listen_buf);
    if (!telemetry)
    {
        cfg_str(rcv, "telemetry", "off");
        cfg_str(sender, "telemetry", "off");
    }
    {
        const char *fec = std::getenv("VSTREAMER_FEC");
        const bool  fec_off =
            nullptr != fec &&
            (0 == std::strcmp(fec, "0") || 0 == std::strcmp(fec, "none"));
        if (!fec_off)
        {
            const char *fk = std::getenv("VSTREAMER_FEC_K");
            const char *fn = std::getenv("VSTREAMER_FEC_N");
            if (cfg_str(sender, "fec", "block") < 0)
            {
                return 1;
            }
            if (nullptr != fk && fk[0] != '\0' && cfg_str(sender, "fec_k", fk) < 0)
            {
                return 1;
            }
            if (nullptr != fn && fn[0] != '\0' && cfg_str(sender, "fec_n", fn) < 0)
            {
                return 1;
            }
            std::fprintf(stderr,
                         "%s: stream_sender FEC block k=%s n=%s (set VSTREAMER_FEC=none for "
                         "plain RTP / camera upstream)\n",
                         app_label, nullptr != fk ? fk : "10", nullptr != fn ? fn : "12");
        }
    }
    cfg_str(dec, "size", size_buf);
    cfg_str(dec, "fps", fps_buf);
    cfg_str(*preview, "title", app_label);

    if (nullptr != std::getenv("VSTREAMER_SKIP_DECODE"))
    {
        g_rx.skip_decode = true;
        std::fprintf(stderr, "%s: decode disabled (VSTREAMER_SKIP_DECODE)\n", app_label);
    }
    if (const char *bench = std::getenv("VSTREAMER_BENCH_METRICS");
        nullptr != bench && bench[0] != '\0' && 0 != std::strcmp(bench, "0"))
    {
        g_bench_metrics_log = true;
    }

    char cbr_bps[24];
    std::snprintf(cbr_bps, sizeof(cbr_bps), "%d", encoder_cbr_kbps * 1000);
    const char *enc_rc = std::getenv("VSTREAMER_ENC_RC");
    if (nullptr == enc_rc || enc_rc[0] == '\0')
    {
        enc_rc = "cbr";
    }
    if (open_stage("h264_encoder cbr", cfg_str(enc, "cbr", cbr_bps)) < 0 ||
        open_stage("h264_encoder rc", cfg_str(enc, "rc", enc_rc)) < 0)
    {
        return 1;
    }

#if defined(ENABLE_V4L2_SOURCE) && defined(ENABLE_NOISE_SOURCE)
    if (use_uvc_selector)
    {
        const auto on_switch = [&](apps::tx::source_kind /*kind*/, int w, int h, int f) {
            char sz[32];
            char fb[16];
            std::snprintf(sz, sizeof(sz), "%dx%d", w, h);
            std::snprintf(fb, sizeof(fb), "%d", f);
            (void)cfg_str(enc, "size", sz);
            (void)cfg_str(enc, "fps", fb);
            (void)cfg_str(pay, "fps", fb);
            if (use_jpeg_decode)
            {
                (void)cfg_str(jdec, "size", sz);
                (void)cfg_str(jdec, "fps", fb);
            }
            (void)enc.configure("idr", std::string_view("1"));
        };
        uvc_selector = std::make_unique<apps::tx::source_selector>(
            camera, noise, width, height, fps, on_switch, apps::tx::source_selector::push_packet_fn {},
            apps::tx::source_selector::push_packet_fn {});
        uvc_metrics_source =
            std::make_unique<apps::tx::source_selector_query_source>(*uvc_selector);
    }
#endif

    const int source_open_rc =
#if defined(ENABLE_V4L2_SOURCE) && defined(ENABLE_NOISE_SOURCE)
        use_uvc_selector ? uvc_selector->open() :
#endif
                           (nullptr != source ? source->open() : -EINVAL);
    if (open_stage(source_open_label, source_open_rc) < 0 ||
        (use_jpeg_decode && open_stage("jpeg_decoder", jdec.open()) < 0) ||
        open_stage("h264_encoder", enc.open()) < 0 || open_stage("stream_sender", sender.open()) < 0 ||
        open_stage("stream_receiver", rcv.open()) < 0)
    {
        return 1;
    }
    {
        std::string max_in;
        if (0 == sender.query("max_input", &max_in) && !max_in.empty())
        {
            char       mtu_cap[24];
            const long cap = std::strtol(max_in.c_str(), nullptr, 10);
            const long configured = std::strtol(mtu_buf, nullptr, 10);
            if (cap > 0 && configured > cap)
            {
                std::snprintf(mtu_cap, sizeof(mtu_cap), "%ld", cap);
                if (cfg_str(pay, "mtu", mtu_cap) < 0)
                {
                    return 1;
                }
            }
        }
    }
    if (open_stage("rtp_h264_pay", pay.open()) < 0 ||
        open_stage("rtp_h264_depay", depay.open()) < 0 ||
        (defer_sdl_to_present ? 0 : open_stage("sdl_sink", preview->open())) < 0)
    {
        return 1;
    }

    if (open_stage("h264_encoder rc", cfg_str(enc, "rc", enc_rc)) < 0)
    {
        return 1;
    }
    {
        const int reported_bps = query_encoder_cbr_bps(enc);
        std::fprintf(stderr, "%s: MPP rc=%s target %d kbps\n", app_label, enc_rc,
                     reported_bps > 0 ? (reported_bps + 500) / 1000 : encoder_cbr_kbps);
    }
    channel.set_encode_command_handlers(
        [](int kbps) -> bool {
            g_tx.pending_console_cbr_kbps.store(kbps, std::memory_order_release);
            return true;
        },
        [](int qp) -> bool {
            g_tx.pending_console_qp.store(qp, std::memory_order_release);
            return true;
        },
        [](int gop) -> bool {
            g_tx.pending_console_gop.store(gop, std::memory_order_release);
            return true;
        },
        []() -> bool {
            g_tx.pending_console_idr.store(true, std::memory_order_release);
            return true;
        });
    channel.set_stream_sender(&sender);

    sender.set_enabled(true, 0);

#if defined(ENABLE_V4L2_SOURCE) && defined(ENABLE_NOISE_SOURCE)
    g_tx.metrics_source =
        use_uvc_selector ? static_cast<component_source *>(uvc_metrics_source.get()) : source;
#else
    g_tx.metrics_source = source;
#endif
    if (!defer_sdl_to_present && prepare_preview_sink(preview, kmsdrm, width, height) < 0)
    {
        std::fprintf(stderr,
                     "stream_sdl: display prepare failed (continuing; first frame creates "
                     "window)\n");
    }
    if (defer_sdl_to_present)
    {
        std::fprintf(stderr,
                     "stream_sdl: SDL window opens on present thread (OpenGL context)\n");
    }

    channel.set_queue_depth(channel_queue);
    channel.set_drop_dt_ms(channel_drop_dt_ms);
    channel.set_bind_host(chan_bind_host);

    const int ch_start =
        channel.start(chan_in, k_loopback_host, rx_port);
    if (ch_start < 0)
    {
        std::fprintf(stderr, "channel_controller start failed (%d", ch_start);
        if (-ch_start > 0 && -ch_start < 4096)
        {
            std::fprintf(stderr, "; %s", std::strerror(-ch_start));
        }
        std::fprintf(stderr, ")\n");
        return 1;
    }
    {
        double chan_cap = test_app::k_chan_default_max_kbps;
        if (const char *env_cap = std::getenv("VSTREAMER_CHAN_MAX_KBPS"))
        {
            const double v = std::strtod(env_cap, nullptr);
            if (v >= 0.0)
            {
                chan_cap = v;
            }
        }
        if (chan_cap > 0.0)
        {
            channel.set_max_kbps(chan_cap);
            std::fprintf(stderr, "stream_sdl: channel forward cap %.0f kbps\n", chan_cap);
        }
    }
    pipeline_rate_state pipeline_rate;
    channel.set_pipeline_metrics(&g_pipeline_metrics);
    const auto refresh_pipeline_metrics = [&, jpeg_active = use_jpeg_decode]() {
        update_pipeline_metrics(g_bench_diag, &enc, &sender, &rcv, preview, kmsdrm, pipeline_rate,
                                &channel, jpeg_active ? &jdec : nullptr, jpeg_active, &dec,
                                &depay);
    };
    channel.set_pipeline_metrics_sync_live([&]() {
        sync_pipeline_metrics_live(g_bench_diag, &sender, &enc, &rcv, &channel);
    });
    channel.set_source_state_metrics_refresh([]() {
        std::string src_state = "running";
        if (nullptr != g_tx.metrics_source)
        {
            (void)query_source_metric_string(g_tx.metrics_source, "state", src_state);
        }
        metric_store(*g_pipeline_metrics.get_metric("source.state"), src_state);
    });
    refresh_pipeline_metrics();
    sync_pipeline_metrics_live(g_bench_diag, &sender, &enc, &rcv, &channel);
    if (channel.start_console(console_port) < 0)
    {
        std::fprintf(stderr, "channel_controller console failed\n");
        return 1;
    }

    std::fprintf(stderr,
                 "%s: %s @ %d fps | source %s | display %s | channel fwd :%d->:%d "
                 "(rev = return path) | telemetry %s | console :%d\n",
                 app_label, size_buf, fps, use_v4l2 ? source_arg : "noise",
                 kmsdrm ? "kmsdrm" : "sdl", chan_in, rx_port, telemetry ? "on" : "off",
                 console_port);
#if defined(ENABLE_NOISE_SOURCE)
    if (!use_v4l2)
    {
        std::string bw_q;
        std::string grid_q;
        std::string simd_q;
        if (query_source_metric_string(&noise, "noise-bandwidth", bw_q) &&
            query_source_metric_string(&noise, "noise-fft-simd", simd_q))
        {
            std::fprintf(stderr, "%s: noise bandwidth=%s fft=%s", app_label, bw_q.c_str(),
                         simd_q.c_str());
            if (query_source_metric_string(&noise, "noise-fft-grid", grid_q))
            {
                std::fprintf(stderr, " grid=%s", grid_q.c_str());
            }
            std::fputc('\n', stderr);
        }
    }
#endif
    apps::stage_latency_set_diag_enabled(diag_log);
    if (diag_log)
    {
        std::fprintf(stderr,
                     "stream_sdl: diagnostic logging enabled (--diag); stage_latency lines on\n");
    }
    g_rx.stream_fps.store(fps, std::memory_order_relaxed);
    const size_t pipe_q_depth = apps::queue_depth_from_env("VSTREAMER_PIPE_QUEUE_DEPTH",
                                                           apps::k_default_pipe_queue_depth, 64);
    const size_t present_q_depth =
        apps::queue_depth_from_env("VSTREAMER_PRESENT_QUEUE_DEPTH",
                                   apps::k_default_present_queue_depth, 16);
    const size_t rx_au_q_depth =
        apps::queue_depth_from_env("VSTREAMER_RX_AU_QUEUE_DEPTH",
                                   apps::k_default_rx_au_queue_depth, 256);
    std::fprintf(stderr,
                 "stream_sdl: queue depths pipe=%zu present=%zu rx_au=%zu "
                 "(override: VSTREAMER_*_QUEUE_DEPTH env)\n",
                 pipe_q_depth, present_q_depth, rx_au_q_depth);
    if (const char *adm = std::getenv("VSTREAMER_ENC_ADMISSION");
        nullptr != adm && adm[0] != '\0' && 0 != std::strcmp(adm, "0"))
    {
        std::fprintf(stderr,
                     "stream_sdl: encode admission pacing ON (VSTREAMER_ENC_ADMISSION); "
                     "MPP CBR only, 1080p+\n");
    }
    apps::pipeline_queue      mjpeg_q(pipe_q_depth, g_run);
    apps::pipeline_queue      nv12_q(pipe_q_depth, g_run);
    g_tx.metrics_nv12_q = &nv12_q;
    apps::present_frame_queue present_q(present_q_depth, g_run);
    apps::rx_au_queue         au_q(rx_au_q_depth, g_run);

    apps::pipeline_controller ctrl;
    ctrl.bind_legacy_run(&g_run);
    ctrl.set_diag_enabled(diag_log);
    const int jpeg_workers = static_cast<int>(g_cpu_map.jpeg_workers.size());

#if defined(ENABLE_V4L2_SOURCE) && defined(ENABLE_NOISE_SOURCE)
    if (use_uvc_selector)
    {
        uvc_selector->set_push_handlers(
            [&](data_packet &&pkt) {
                (void)enqueue_source_frame(std::move(pkt), &mjpeg_q, &nv12_q, &g_bench_diag);
            },
            [&](data_packet &&pkt) {
                (void)enqueue_source_frame(std::move(pkt), &mjpeg_q, &nv12_q, &g_bench_diag);
            });
        ctrl.add_stage("source", "source",
                       [&, sel = uvc_selector.get()](std::atomic<bool> & /*run*/) {
                           source_stage_selector_main(sel, &mjpeg_q, &nv12_q, &g_bench_diag);
                       });
    }
    else
#endif
    {
        ctrl.add_stage("source", "source",
                       [&, src = source](std::atomic<bool> & /*run*/) {
                           source_stage_main(src, &mjpeg_q, &nv12_q, &g_bench_diag);
                       });
    }
    if (use_jpeg_decode)
    {
        ctrl.add_stage("jpeg", "jpeg", [&, jw = jpeg_workers](std::atomic<bool> & /*run*/) {
            jpeg_stage_main(&jdec, &mjpeg_q, &nv12_q, &g_bench_diag, jw);
        });
    }
    ctrl.add_stage("encode", "encode", [&](std::atomic<bool> & /*run*/) {
        encode_stage_main(&enc, &pay, &sender, &nv12_q, &g_bench_diag);
    });
    ctrl.add_stage("telemetry", "",
                   [&](std::atomic<bool> & /*run*/) { apps::tx::telemetry_thread_main(&sender); });
    ctrl.add_metrics_sync([&]() {
        static int bench_log_ticks = 0;
        refresh_pipeline_metrics();
        if (g_bench_metrics_log.load())
        {
            ++bench_log_ticks;
            if (bench_log_ticks >= 5)
            {
                bench_log_ticks = 0;
                log_bench_rate_line(pipeline_rate, enc, g_bench_diag);
            }
        }
    });
    ctrl.add_stage("present", "",
                   [&](std::atomic<bool> & /*run*/) {
                       present_thread_main(preview, &present_q, width, height, kmsdrm,
                                           defer_sdl_to_present, &g_bench_diag);
                   });
    ctrl.add_stage("rx_net", "rx", [&](std::atomic<bool> & /*run*/) {
        rx_net_thread_main(&rcv, &depay, &au_q, &g_bench_diag);
    });
    ctrl.add_stage("decode", "rx", [&](std::atomic<bool> & /*run*/) {
        decode_thread_main(&dec, &present_q, &au_q, &g_bench_diag);
    });
    if (!self_test && diag_log)
    {
        ctrl.add_stage("diag", "",
                       [&](std::atomic<bool> &run) {
                           while (run.load())
                           {
                               log_bench_diag(g_bench_diag, rcv, sender, enc, &channel);
                               for (int i = 0; i < 20 && run.load(); ++i)
                               {
                                   std::this_thread::sleep_for(std::chrono::milliseconds(100));
                               }
                           }
                       });
    }

    bool self_test_ok = true;
    if (self_test)
    {
        std::thread runner([&]() { ctrl.run(); });
        self_test_ok = run_self_test(enc, preview, rcv, sender, console_port, &channel);
        ctrl.request_stop();
        runner.join();
    }
    else
    {
        ctrl.run();
    }

    shutdown_pipeline(enc, dec, sender, rcv, use_jpeg_decode ? &jdec : nullptr,
                      use_v4l2 ? &camera : nullptr, use_jpeg_decode ? &mjpeg_q : nullptr,
                      &nv12_q);
    present_q.wake();
    au_q.wake();
    mjpeg_q.wake_shutdown();
    nv12_q.wake_shutdown();
    channel.stop();
    source->close();
    if (use_jpeg_decode)
    {
        jdec.close();
    }

    if (diag_log)
    {
        log_bench_diag(g_bench_diag, rcv, sender, enc, &channel);
    }

    preview->close();
    dec.close();
    depay.close();
    pay.close();
    enc.close();

    if (self_test && !self_test_ok)
    {
        return 1;
    }
    return 0;
}
