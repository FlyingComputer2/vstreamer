// Smoke tests for the vstreamer component on ESP-IDF.
//
// 1. Loopback:
//
//   stream_sender (tx) -> UDP 127.0.0.1:5600 -> [pipeline: stream_receiver -> stream_sender]
//                      -> UDP 127.0.0.1:5601 -> stream_receiver (rx)
//
// Runs on a pthread (see app_main). Runs every thread the IDF build has (pipeline, send, telemetry, receive, mkv mux), counts
// the datagrams that arrive at rx and prints each thread's unused stack.
//
// 2. h264_encoder_esp32p4 (esp32p4 only): encodes synthetic O_UYY_E_VYY frames at 1080p for
// timing, then a short 320x240 clip that it prints as H264_BEGIN / base64 lines / H264_END so
// the host can decode it.
//
// 3. esp32p4_csi_camera_src -> h264_encoder_esp32p4 (esp32p4 only): captures from the MIPI-CSI
// sensor in sdkconfig for a few seconds, reports fps and dumps the first second as a clip.
// Skipped when no sensor answers on SCCB.
//
// 4. Live TX (CONFIG_VSTREAMER_IDF_LIVE_TX): h264_isp_src -> rtp_h264_pay -> stream_sender
//    to CONFIG_VSTREAMER_RX_PEER over Ethernet (DHCP). Skips tests 1–3.

#include "ethernet_init.hpp"
#include "vstreamer_idf/live_tx.hpp"
#include "sdkconfig.h"

#include "components/components_config.hpp"
#if defined(ENABLE_ESP32P4_CSI_CAMERA_SRC)
#include "components/esp32p4_csi_camera_src.hpp"
#endif
#include "core/component_coder.hpp"
#include "core/component_factory.hpp"
#include "core/rs_encode_scalar.hpp"
#include "core/sdu_caps.hpp"
#include "core/component_pdu.hpp"
#include "core/component_sink.hpp"
#include "core/component_source.hpp"
#include "core/pipeline.hpp"
#include <bfc/shared_sized_buffer.hpp>
#include "core/thread_spawn.hpp"
#include "core/time_util.hpp"

#include "esp_cache.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <cinttypes>
#include <climits>
#include <atomic>
#include <condition_variable>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{

constexpr int k_datagrams = 300;
constexpr int k_payload_bytes = 1000;

/* Thread placement on the P4's two cores for the smoke tests ("<role>_cpu" / "<role>_prio";
 * components default to any core). Live TX has its own layout in vstreamer_idf/live_tx. */
struct place_kv
{
    const char *key;
    const char *value;
};

/* Smoke tests. */
constexpr place_kv k_camera_place[] = {{"capture_cpu", "0"}, {"capture_prio", "12"}};
constexpr place_kv k_sender_place[] = {{"send_cpu", "0"}, {"send_prio", "12"}};

template <size_t N>
void apply_place(vstreamer::component &c, const place_kv (&kvs)[N])
{
    for (const auto &kv : kvs)
    {
        if (c.configure(kv.key, kv.value) < 0)
        {
            std::printf("placement %s=%s rejected\n", kv.key, kv.value);
        }
    }
}

void print_heap(const char *when)
{
    std::printf("heap %-16s internal %u (largest %u), psram %u\n", when,
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

void print_stack_left(const char *task)
{
    TaskHandle_t h = xTaskGetHandle(task);
    if (nullptr == h)
    {
        std::printf("  %-12s not running\n", task);
        return;
    }
    std::printf("  %-12s %u bytes unused\n", task,
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(h)));
}

void print_query(const char *who, const vstreamer::component &c, const char *key)
{
    std::string v;
    if (c.query(key, &v) == 0)
    {
        std::printf("  %s %s = %s\n", who, key, v.c_str());
    }
}

#ifdef ENABLE_H264_ENCODER_ESP32P4

/* Moving diagonal gradient in the P4 ISP layout: line pairs U Y Y ... / V Y Y ... */
void fill_frame(uint8_t *p, int w, int h, int frame)
{
    const int line_bytes = w * 3 / 2;
    for (int y = 0; y < h; ++y)
    {
        uint8_t      *row = p + static_cast<size_t>(y) * line_bytes;
        const uint8_t c = (y & 1) == 0 ? static_cast<uint8_t>(128 + (y * 64) / h)   /* U */
                                       : static_cast<uint8_t>(192 - (y * 64) / h);  /* V */
        for (int x = 0; x < w; x += 2)
        {
            row[0] = c;
            row[1] = static_cast<uint8_t>(x + y + 4 * frame);
            row[2] = static_cast<uint8_t>(x + 1 + y + 4 * frame);
            row += 3;
        }
    }
}

void dump_clip(const std::string &clip, int frames)
{
    std::printf("H264_BEGIN frames=%d bytes=%u\n", frames, static_cast<unsigned>(clip.size()));
    unsigned char line[80];
    for (size_t off = 0; off < clip.size(); off += 57)
    {
        const size_t n = clip.size() - off < 57 ? clip.size() - off : 57;
        size_t       olen = 0;
        mbedtls_base64_encode(line, sizeof(line) - 1, &olen,
                              reinterpret_cast<const unsigned char *>(clip.data()) + off, n);
        line[olen] = 0;
        std::printf("H264 %s\n", line);
        if ((off / 57) % 64 == 63)
        {
            std::fflush(stdout);
            vTaskDelay(1);
        }
    }
    std::printf("H264_END\n");
    std::fflush(stdout);
}

/* Encodes `frames` frames; appends the bitstream to clip when non-null. */
int encode_frames(int w, int h, int frames, int gop, std::string *clip)
{
    auto enc = vstreamer::component_factory::create_coder("h264_encoder");
    if (!enc)
    {
        std::printf("h264_encoder not in the build\n");
        return -1;
    }
    char size[32];
    std::snprintf(size, sizeof(size), "%dx%d", w, h);
    enc->configure("size", size);
    enc->configure("gop", std::to_string(gop));
    if (enc->open() < 0)
    {
        std::printf("encoder open %s failed\n", size);
        return -1;
    }
    vstreamer::video_raw_caps caps {};
    caps.width = w;
    caps.height = h;
    caps.fps_num = 30;
    caps.fps_den = 1;
    if (enc->input(vstreamer::make_caps_pdu(vstreamer::sdu_type_e::CAPS_VIDEO_RAW, caps, 0, 0)) < 0)
    {
        std::printf("encoder rejected caps\n");
        return -1;
    }

    const size_t frame_bytes = static_cast<size_t>(w) * h * 3 / 2;
    int          aus = 0;
    int          keys = 0;
    size_t       bytes = 0;
    const int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < frames; ++i)
    {
        vstreamer::component_pdu in;
        in.ts_us = static_cast<uint64_t>(i) * 33333;
        in.sdu_type = vstreamer::sdu_type_e::O_UYY_E_VYY;
        in.sdu = bfc::shared_sized_buffer::allocate(frame_bytes);
        in.sdu.resize(frame_bytes);
        fill_frame(in.sdu.u8(), w, h, i);
        const int r = enc->input(std::move(in));
        if (r < 0)
        {
            std::printf("encode frame %d failed: %d\n", i, r);
            return -1;
        }
        vstreamer::component_pdu out;
        while (enc->output(out) == 0)
        {
            if (out.sdu_type != vstreamer::sdu_type_e::H264_AU)
            {
                continue;
            }
            ++aus;
            bytes += out.sdu.size();
            if (vstreamer::has_flag(out, vstreamer::pdu_flag_e::KEY))
            {
                ++keys;
            }
            if (nullptr != clip)
            {
                clip->append(reinterpret_cast<const char *>(out.sdu.u8()), out.sdu.size());
            }
        }
    }
    const double sec = (esp_timer_get_time() - t0) / 1e6;
    std::string  stats;
    enc->query("stats", &stats);
    std::printf("encode %s: %d AUs, %d IDR, %u bytes, %.1f fps incl. frame fill; %s\n", size, aus,
                keys, static_cast<unsigned>(bytes), frames / sec, stats.c_str());
    enc->close();
    return aus == frames ? 0 : -1;
}

vstreamer::component_pdu raw_frame(int w, int h, int i)
{
    const size_t             bytes = static_cast<size_t>(w) * h * 3 / 2;
    vstreamer::component_pdu in;
    in.ts_us = static_cast<uint64_t>(i) * 33333;
    in.sdu_type = vstreamer::sdu_type_e::O_UYY_E_VYY;
    in.sdu = bfc::shared_sized_buffer::allocate(bytes);
    in.sdu.resize(bytes);
    fill_frame(in.sdu.u8(), w, h, i);
    return in;
}

vstreamer::component_pdu raw_caps(int w, int h)
{
    vstreamer::video_raw_caps caps {};
    caps.width = w;
    caps.height = h;
    caps.fps_num = 30;
    caps.fps_den = 1;
    return vstreamer::make_caps_pdu(vstreamer::sdu_type_e::CAPS_VIDEO_RAW, caps, 0, 0);
}

/* A full output queue pushes back (-EAGAIN) and keeps the caps PDU first; a runtime resize
 * sends coded caps of the new size before its first AU. */
int encoder_runtime_checks()
{
    auto enc = vstreamer::component_factory::create_coder("h264_encoder");
    enc->configure("size", "320x240");
    if (!enc || enc->open() < 0 || enc->input(raw_caps(320, 240)) < 0)
    {
        return -1;
    }
    int accepted = 0;
    int r = 0;
    while ((r = enc->input(raw_frame(320, 240, accepted))) == 0 && accepted < 20)
    {
        ++accepted;
    }
    vstreamer::component_pdu out;
    const bool caps_first = enc->output(out) == 0 &&
                            out.sdu_type == vstreamer::sdu_type_e::CAPS_VIDEO_CODED;
    std::printf("backpressure: %d frames accepted, then %d; caps first: %s\n", accepted, r,
                caps_first ? "yes" : "no");
    if (r != -EAGAIN || !caps_first)
    {
        return -1;
    }
    while (enc->output(out) == 0)
    {
    }

    enc->configure("size", "160x120");
    if (enc->input(raw_caps(160, 120)) < 0 || enc->input(raw_frame(160, 120, 0)) < 0)
    {
        std::printf("resize: input failed\n");
        return -1;
    }
    vstreamer::video_coded_caps coded {};
    const bool caps_ok = enc->output(out) == 0 && vstreamer::read_caps(out, &coded) == 0 &&
                         160 == coded.width && 120 == coded.height;
    const bool au_ok = enc->output(out) == 0 &&
                       out.sdu_type == vstreamer::sdu_type_e::H264_AU &&
                       vstreamer::has_flag(out, vstreamer::pdu_flag_e::KEY);
    std::printf("resize: new caps %dx%d first: %s, then IDR AU: %s\n",
                static_cast<int>(coded.width), static_cast<int>(coded.height),
                caps_ok ? "yes" : "no", au_ok ? "yes" : "no");
    enc->close();
    return caps_ok && au_ok ? 0 : -1;
}

int run_encoder()
{
    print_heap("encoder start");
    int r = encode_frames(1920, 1080, 60, 30, nullptr);
    std::string clip;
    if (0 == r)
    {
        r = encode_frames(320, 240, 30, 10, &clip);
    }
    if (0 == r)
    {
        r = encoder_runtime_checks();
    }
    print_heap("encoder end");
    if (0 == r)
    {
        dump_clip(clip, 30);
    }
    std::printf("encoder test %s\n", 0 == r ? "PASSED" : "FAILED");
    return r;
}

#endif  // ENABLE_H264_ENCODER_ESP32P4

#if defined(ENABLE_ESP32P4_CSI_CAMERA_SRC) && defined(ENABLE_H264_ENCODER_ESP32P4)

int run_camera()
{
    constexpr int     k_seconds = 5;
    constexpr int     k_clip_frames = 30;
    /* Printed base64 over the console UART; a noisy 1080p scene codes at ~100 KB a frame until
     * rate control settles. */
    constexpr size_t  k_clip_max_bytes = 256 * 1024;
    auto              cam = vstreamer::component_factory::create_source("csi_camera");
    auto              enc = vstreamer::component_factory::create_coder("h264_encoder");
    if (!cam || !enc)
    {
        std::printf("camera or encoder not in the build\n");
        return -1;
    }
    print_heap("camera start");
    apply_place(*cam, k_camera_place);
    const int cr = cam->open();
    if (-ENODEV == cr)
    {
        std::printf("camera test SKIPPED (no sensor)\n");
        return 0;
    }
    if (cr < 0)
    {
        std::printf("camera open failed: %d\n", cr);
        return -1;
    }
    std::string size;
    cam->query("size", &size);
    enc->configure("size", size);
    if (enc->open() < 0)
    {
        std::printf("encoder open %s failed\n", size.c_str());
        return -1;
    }

    int           frames = 0;
    int           keys = 0;
    int           clip_frames = 0;
    size_t        bytes = 0;
    std::string   clip;
    clip.reserve(k_clip_max_bytes);
    int64_t       t0 = 0;
    const int64_t deadline = esp_timer_get_time() + (k_seconds + 1) * 1000000LL;
    while (esp_timer_get_time() < deadline)
    {
        vstreamer::component_pdu in;
        if (cam->output(in) != 0)
        {
            vTaskDelay(1);
            continue;
        }
        if (in.sdu_type == vstreamer::sdu_type_e::O_UYY_E_VYY && 0 == t0)
        {
            /* Rate from the first frame, past sensor start-up. */
            t0 = esp_timer_get_time();
        }
        const int r = enc->input(std::move(in));
        if (r < 0)
        {
            std::printf("encode failed: %d\n", r);
            break;
        }
        vstreamer::component_pdu out;
        while (enc->output(out) == 0)
        {
            if (out.sdu_type != vstreamer::sdu_type_e::H264_AU)
            {
                continue;
            }
            ++frames;
            bytes += out.sdu.size();
            const bool key = vstreamer::has_flag(out, vstreamer::pdu_flag_e::KEY);
            keys += key ? 1 : 0;
            /* Start the clip on an IDR so it decodes on its own. */
            if (clip_frames < k_clip_frames && (clip_frames > 0 || key) &&
                clip.size() + out.sdu.size() <= k_clip_max_bytes)
            {
                clip.append(reinterpret_cast<const char *>(out.sdu.u8()), out.sdu.size());
                ++clip_frames;
            }
        }
        if (t0 > 0 && esp_timer_get_time() - t0 >= k_seconds * 1000000LL)
        {
            break;
        }
    }
    const double sec = t0 > 0 ? (esp_timer_get_time() - t0) / 1e6 : 0;
    std::string  cam_stats;
    std::string  enc_stats;
    cam->query("stats", &cam_stats);
    enc->query("stats", &enc_stats);
    std::printf("camera %s: %d AUs, %d IDR, %.1f fps, %.0f kbps; cam %s; enc %s\n", size.c_str(),
                frames, keys, sec > 0 ? frames / sec : 0.0, sec > 0 ? bytes * 8 / sec / 1000 : 0.0,
                cam_stats.c_str(), enc_stats.c_str());
    enc->close();
    cam->close();
    print_heap("camera end");
    const bool ok = sec > 0 && frames / sec >= 20;
    if (ok)
    {
        dump_clip(clip, clip_frames);
    }
    std::printf("camera test %s\n", ok ? "PASSED" : "FAILED");
    return ok ? 0 : -1;
}

#endif

int run()
{
    using vstreamer::component_factory;

    print_heap("start");
    auto rx = component_factory::create_source("stream_receiver");
    print_heap("after rx");
    auto tx = component_factory::create_sink("stream_sender");
    print_heap("after tx");
    auto mkv = component_factory::create_sink("mkv_sink");
    print_heap("after mkv");
    if (!rx || !tx || !mkv)
    {
        std::printf("component missing from the build\n");
        return -1;
    }

    rx->configure("listen", "127.0.0.1:5601");
    if (rx->open() < 0)
    {
        std::printf("rx open failed\n");
        return -1;
    }

    vstreamer::pipeline relay("relay");
    relay.add_source("in", "stream_receiver");
    relay.add_sink("out", "stream_sender");
    relay.configure("in", "listen", "127.0.0.1:5600");
    relay.configure("out", "stream", "127.0.0.1:5601");
    apply_place(*relay.node("out"), k_sender_place);
    relay.link("in", "out");
    /* stream_sender starts with its send gate closed. */
    static_cast<vstreamer::component_sink *>(relay.node("out"))->set_enabled(true, 0);
    relay.set_cpu(1);
    const int sr = relay.start();
    if (sr < 0)
    {
        std::printf("relay start failed: %d\n", sr);
        return -1;
    }

    tx->configure("stream", "127.0.0.1:5600");
    apply_place(*tx, k_sender_place);
    tx->set_enabled(true, 0);
    if (tx->open() < 0)
    {
        std::printf("tx open failed\n");
        return -1;
    }

    /* Starts the mux thread; nothing is recorded without an "output". */
    (void)mkv->open();

    uint8_t payload[k_payload_bytes];
    for (int i = 0; i < k_datagrams; ++i)
    {
        std::memset(payload, i & 0xFF, sizeof(payload));
        vstreamer::component_pdu pdu;
        pdu.sdu_type = vstreamer::sdu_type_e::STREAM_DGRAM;
        pdu.sdu = bfc::shared_sized_buffer::copy_from(payload, sizeof(payload));
        while (tx->input(std::move(pdu)) == -EAGAIN)
        {
            vTaskDelay(pdMS_TO_TICKS(1));
            pdu.sdu_type = vstreamer::sdu_type_e::STREAM_DGRAM;
            pdu.sdu = bfc::shared_sized_buffer::copy_from(payload, sizeof(payload));
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    int received = 0;
    for (int waited = 0; waited < 2000 && received < k_datagrams; waited += 10)
    {
        vstreamer::component_pdu out;
        while (rx->output(out) == 0)
        {
            ++received;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    std::printf("received %d / %d datagrams through the relay pipeline\n", received,
                k_datagrams);
    print_query("tx", *tx, "stats");
    for (const char *k : {"udp_packet_received", "fec_packet_received", "rx_truncated",
                          "rx_oversize", "rx_bad_header", "stats"})
    {
        print_query("relay.in", *relay.node("in"), k);
    }
    print_query("rx", *rx, "fec_packet_received");
    print_query("rx", *rx, "fec_recovered");

    std::printf("thread stacks (high-water mark):\n");
    for (const char *t : {"smoke", "vs_pipeline", "vs_send", "vs_telemetry", "vs_recv", "vs_mkv"})
    {
        print_stack_left(t);
    }

    mkv->close();
    tx->close();
    relay.stop();
    rx->close();
    std::printf("smoke test %s\n", received == k_datagrams ? "PASSED" : "FAILED");
    return received == k_datagrams ? 0 : -1;
}

}  // namespace

extern "C" void app_main(void)
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    if (vstreamer_idf_ethernet_start() != ESP_OK)
    {
        std::printf("Ethernet start failed\n");
    }
    else if (vstreamer_idf_wait_for_ip(30000) != ESP_OK)
    {
        std::printf("DHCP timeout (continuing anyway)\n");
    }
    /* pipeline control uses std::this_thread::get_id(), which needs a pthread; app_main is a
     * plain FreeRTOS task. */
    auto t = vstreamer::spawn_thread(vstreamer::thread_opts {"smoke", 24 * 1024}, [] {
        {
            /* FEC encode check: the fast path (PIE on the P4) against the table path, misaligned
             * shards, with and without a tail. A mismatch turns the fast path off. */
            const auto now = [] { return static_cast<int64_t>(esp_timer_get_time()); };
            for (const int len : {1403, 37})
            {
                int64_t   fast = 0;
                int64_t   table = 0;
                const int bad = vstreamer::rs_encode_selftest(21, 10, len, 2, now, &fast, &table);
                if (bad > 0)
                {
                    std::printf("RS ENCODE SELFTEST FAILED (len %d, %d parity rows differ): FEC encode "
                                "falls back to the table path\n",
                                len, bad);
                }
                else
                {
                    std::printf("rs selftest len=%d ok: %s %" PRId64 " us, table %" PRId64 " us\n", len,
                                vstreamer::rs_encode_scalar_impl(), fast, table);
                }
            }
        }
#if defined(ENABLE_ESP32P4_CSI_CAMERA_SRC) && defined(ENABLE_H264_ENCODER_ESP32P4) && \
    CONFIG_VSTREAMER_IDF_LIVE_TX
        vstreamer::idf::live_tx_config cfg;
        cfg.peer = CONFIG_VSTREAMER_RX_PEER;
        cfg.seconds = CONFIG_VSTREAMER_LIVE_TX_SECONDS;
        cfg.mplane_port = CONFIG_VSTREAMER_LIVE_TX_MPLANE_PORT;
        while (true)
        {
            (void)vstreamer::idf::run_live_tx(cfg);
            std::printf("live TX restarting in 3 s\n");
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
#else
        (void)run();
#ifdef ENABLE_H264_ENCODER_ESP32P4
        (void)run_encoder();
#endif
#if defined(ENABLE_ESP32P4_CSI_CAMERA_SRC) && defined(ENABLE_H264_ENCODER_ESP32P4)
        (void)run_camera();
#endif
#endif
    });
    t.join();
}
