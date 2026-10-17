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
#include "live_mplane.hpp"
#include "sdkconfig.h"

#include "components/components_config.hpp"
#if defined(ENABLE_ESP32P4_CSI_CAMERA_SRC)
#include "components/esp32p4_csi_camera_src.hpp"
#endif
#include "core/component_coder.hpp"
#include "core/component_factory.hpp"
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
#include "esp_pm.h"
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

#if defined(ENABLE_ESP32P4_CSI_CAMERA_SRC) && defined(ENABLE_H264_ENCODER_ESP32P4) && \
    CONFIG_VSTREAMER_IDF_LIVE_TX

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
/* Per-caller window for sys_stats_line(), so the stats print and the mplane do not share one. */
struct cpu_window
{
    struct prev_t
    {
        std::string name;
        uint32_t    rt;
    };
    std::vector<prev_t> prev;
    int64_t             prev_us = 0;
};

/* CPU per task since the previous call on w (percent of one core), then heap headroom. */
std::string sys_stats_line(cpu_window *w)
{
    using prev_t = cpu_window::prev_t;
    std::vector<prev_t> &prev = w->prev;
    int64_t             &prev_us = w->prev_us;
    const UBaseType_t          n = uxTaskGetNumberOfTasks() + 4;
    std::vector<TaskStatus_t>  st(n);
    configRUN_TIME_COUNTER_TYPE total = 0;
    const UBaseType_t got = uxTaskGetSystemState(st.data(), n, &total);
    const int64_t     now = esp_timer_get_time();
    const double      dt = prev_us > 0 ? static_cast<double>(now - prev_us) : 0;
    std::string       line;
    std::vector<prev_t> cur;
    for (UBaseType_t i = 0; i < got; ++i)
    {
        const std::string name = st[i].pcTaskName;
        const uint32_t    rt = st[i].ulRunTimeCounter;
        cur.push_back({name, rt});
        for (const auto &p : prev)
        {
            if (p.name == name && dt > 0)
            {
                const double pct = 100.0 * static_cast<uint32_t>(rt - p.rt) / dt;
                if (pct >= 0.5 || 0 == name.rfind("IDLE", 0))
                {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), " %s=%.1f%%", name.c_str(), pct);
                    line += buf;
                }
                break;
            }
        }
    }
    prev = std::move(cur);
    prev_us = now;
    char heap[160];
    std::snprintf(heap, sizeof(heap), "; int free=%u min=%u big=%u; psram free=%u min=%u big=%u",
                  heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                  heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
                  heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    return "cpu" + line + heap;
}

void print_sys_stats()
{
    static cpu_window w;
    std::printf("sys %s\n", sys_stats_line(&w).c_str());
}
#endif

void drain_pay_to_sender(vstreamer::component_coder *pay, vstreamer::component_sink *sender)
{
    vstreamer::component_pdu out;
    while (pay->output(out) == 0)
    {
        while (true)
        {
            const int r = sender->input(std::move(out));
            if (0 == r)
            {
                break;
            }
            if (-EAGAIN != r)
            {
                std::printf("stream_sender input failed: %d\n", r);
                return;
            }
            vTaskDelay(1);
        }
    }
}

/* Camera DQBUF to the AU's last datagram handed to stream_sender, per stats interval. */
std::atomic<int64_t> g_age_sum_us {0};
std::atomic<int64_t> g_age_max_us {0};
std::atomic<int64_t> g_age_n {0};
std::atomic<int64_t> g_age_in_sum_us {0}; /* DQBUF to the TX thread getting the AU */

bool forward_coded_output(vstreamer::component_coder *pay, vstreamer::component_sink *sender,
                          vstreamer::component_pdu &&out, size_t *au_bytes, bool *key,
                          std::atomic<uint64_t> *frames, std::atomic<uint64_t> *bytes,
                          std::atomic<uint64_t> *keys)
{
    if (out.sdu_type == vstreamer::sdu_type_e::CAPS_VIDEO_CODED)
    {
        for (;;)
        {
            const int r = pay->input(std::move(out));
            if (0 == r)
            {
                return true;
            }
            if (-EAGAIN != r)
            {
                std::printf("pay caps failed: %d\n", r);
                return false;
            }
            drain_pay_to_sender(pay, sender);
            vTaskDelay(1);
        }
    }
    if (out.sdu_type != vstreamer::sdu_type_e::H264_AU)
    {
        return true;
    }
    if (nullptr != au_bytes)
    {
        *au_bytes = out.sdu.size();
    }
    if (nullptr != key)
    {
        *key = vstreamer::has_flag(out, vstreamer::pdu_flag_e::KEY);
    }
    const int64_t capture_us = static_cast<int64_t>(out.ts_us);
    g_age_in_sum_us.fetch_add(vstreamer::steady_mono_ns() / 1000 - capture_us,
                              std::memory_order_relaxed);
    for (;;)
    {
        drain_pay_to_sender(pay, sender);
        const int r = pay->input(std::move(out));
        if (0 == r)
        {
            break;
        }
        if (-EAGAIN != r)
        {
            std::printf("pay failed: %d\n", r);
            return false;
        }
        vTaskDelay(1);
    }
    drain_pay_to_sender(pay, sender);
    const int64_t age = vstreamer::steady_mono_ns() / 1000 - capture_us;
    g_age_sum_us.fetch_add(age, std::memory_order_relaxed);
    g_age_n.fetch_add(1, std::memory_order_relaxed);
    if (age > g_age_max_us.load(std::memory_order_relaxed))
    {
        g_age_max_us.store(age, std::memory_order_relaxed);
    }
    if (nullptr != frames)
    {
        frames->fetch_add(1, std::memory_order_relaxed);
    }
    if (nullptr != bytes && nullptr != au_bytes)
    {
        bytes->fetch_add(*au_bytes, std::memory_order_relaxed);
    }
    if (nullptr != keys && key && *key)
    {
        keys->fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

int run_camera_stream()
{
    const int k_seconds = CONFIG_VSTREAMER_LIVE_TX_SECONDS;
    const bool  run_forever = (0 == k_seconds);

    auto src = vstreamer::component_factory::create_source("h264_isp_src");
    auto pay = vstreamer::component_factory::create_coder("rtp_h264_pay");
    auto snd = vstreamer::component_factory::create_sink("stream_sender");
    auto *pay_c = dynamic_cast<vstreamer::component_coder *>(pay.get());
    auto *snd_s = dynamic_cast<vstreamer::component_sink *>(snd.get());
    if (!src || !pay_c || !snd_s)
    {
        std::printf("live TX: missing component\n");
        return -1;
    }

    print_heap("live start");
    esp_pm_lock_handle_t pm_lock = nullptr;
    if (esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "live_tx", &pm_lock) == ESP_OK)
    {
        esp_pm_lock_acquire(pm_lock);
    }
    (void)src->configure("buffers", "6");
    (void)src->configure("fps", "30");
    (void)src->configure("bitrate", "10000000");
    (void)src->configure("gop", "30");
    (void)src->configure("qp_min", "20");
    (void)src->configure("qp_max", "51");

    const int cr = src->open();
    if (-ENODEV == cr)
    {
        std::printf("live TX SKIPPED (no sensor)\n");
        return 0;
    }
    if (cr < 0)
    {
        std::printf("camera open failed: %d\n", cr);
        return -1;
    }

    std::string size;
    src->query("size", &size);
    pay->configure("fps", "30");
    pay->configure("mtu", "1400");
    snd->configure("stream", CONFIG_VSTREAMER_RX_PEER);
    snd->configure("mtu", "1400");
    snd->configure("max_kbps", "0");
    snd->configure("telemetry", "off");
    snd->configure("fec", "none");

    if (pay->open() < 0 || snd->open() < 0)
    {
        std::printf("live TX open failed\n");
        return -1;
    }
    snd_s->set_enabled(true, 0);

    /* Runtime control (FEC k/n, bitrate, ...) for bench sweeps. */
    live_mplane mplane;
#if CONFIG_VSTREAMER_LIVE_TX_MPLANE_PORT > 0
    mplane.add_node("src", src.get());
    mplane.add_node("pay", pay.get());
    mplane.add_node("snd", snd.get());
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    mplane.add_query("sys", "stats", [] {
        static cpu_window w;
        return sys_stats_line(&w);
    });
#endif
    if (const int mr = mplane.start(CONFIG_VSTREAMER_LIVE_TX_MPLANE_PORT); mr < 0)
    {
        std::printf("live TX mplane: bind :%d failed (%d)\n", CONFIG_VSTREAMER_LIVE_TX_MPLANE_PORT,
                    mr);
    }
    else
    {
        std::printf("live TX mplane on udp :%d\n", CONFIG_VSTREAMER_LIVE_TX_MPLANE_PORT);
    }
#endif

    if (run_forever)
    {
        std::printf("live TX streaming %s until reset (size %s, h264_isp_src)\n",
                    CONFIG_VSTREAMER_RX_PEER, size.c_str());
    }
    else
    {
        std::printf("live TX streaming %s for %d s (size %s, h264_isp_src)\n",
                    CONFIG_VSTREAMER_RX_PEER, k_seconds, size.c_str());
    }
    std::fflush(stdout);

    std::atomic<uint64_t> frames {0};
    std::atomic<uint64_t> keys {0};
    std::atomic<uint64_t> bytes {0};
    std::atomic<int64_t>  t0_us {0};
    std::atomic<bool>     stop {false};

    const int64_t deadline =
        run_forever ? INT64_MAX : esp_timer_get_time() + (k_seconds + 5) * 1000000LL;
    const int64_t stop_at =
        run_forever ? INT64_MAX : esp_timer_get_time() + k_seconds * 1000000LL;
    int64_t       last_stats_us = esp_timer_get_time();
    uint64_t      last_enc_frames = 0;

    auto tx_fn = [&]() {
        while (!stop.load(std::memory_order_acquire))
        {
            vstreamer::component_pdu out;
            const int              r = src->output(out);
            if (-EAGAIN == r)
            {
                vTaskDelay(1);
                continue;
            }
            if (r < 0)
            {
                vTaskDelay(1);
                continue;
            }
            if (out.sdu_type == vstreamer::sdu_type_e::H264_AU &&
                0 == t0_us.load(std::memory_order_relaxed))
            {
                t0_us.store(esp_timer_get_time(), std::memory_order_relaxed);
            }
            size_t au_bytes = 0;
            bool   key = false;
            if (!forward_coded_output(pay_c, snd_s, std::move(out), &au_bytes, &key, &frames,
                                      &bytes, &keys))
            {
                stop.store(true, std::memory_order_release);
            }
        }
    };

    auto tx_thread =
        vstreamer::spawn_thread(vstreamer::thread_opts {"vs_isp_tx", 16 * 1024, 0, 13}, tx_fn);

    while (!stop.load(std::memory_order_acquire) && esp_timer_get_time() < deadline)
    {
        if (esp_timer_get_time() - last_stats_us >= 10 * 1000000LL)
        {
            const int64_t  now_us = esp_timer_get_time();
            const double   stat_dt = (now_us - last_stats_us) / 1e6;
            last_stats_us = now_us;
            std::string isp_stats;
            std::string snd_stats;
            std::string enc_frames_s;
            src->query("stats", &isp_stats);
            snd->query("stats", &snd_stats);
            (void)src->query("enc_frames", &enc_frames_s);
            uint64_t enc_total = 0;
            if (!enc_frames_s.empty())
            {
                enc_total = std::strtoull(enc_frames_s.c_str(), nullptr, 10);
            }
            const double enc_fps =
                stat_dt > 0 && enc_total >= last_enc_frames
                    ? static_cast<double>(enc_total - last_enc_frames) / stat_dt
                    : 0.0;
            last_enc_frames = enc_total;
            const int64_t  t0 = t0_us.load(std::memory_order_relaxed);
            const double   sec = t0 > 0 ? (esp_timer_get_time() - t0) / 1e6 : 0;
            const uint64_t f = frames.load(std::memory_order_relaxed);
            const uint64_t b = bytes.load(std::memory_order_relaxed);
            const int64_t age_n = g_age_n.exchange(0);
            const int64_t age_sum = g_age_sum_us.exchange(0);
            const int64_t age_max = g_age_max_us.exchange(0);
            const int64_t age_in = g_age_in_sum_us.exchange(0);
            std::string lat;
            std::string node_lat;
            (void)src->query("latency_ms", &lat);
            (void)src->query("node_latency_ms", &node_lat);
            std::printf("live TX %.1f fps (enc %.1f) %.0f kbps age_ms=%.1f/%.1f (at tx %.1f) "
                        "latency_ms=%s node_latency_ms=%s; isp %s; snd %s\n",
                        sec > 0 ? f / sec : 0.0, enc_fps, sec > 0 ? b * 8 / sec / 1000 : 0.0,
                        age_n > 0 ? age_sum / 1000.0 / age_n : 0.0, age_max / 1000.0,
                        age_n > 0 ? age_in / 1000.0 / age_n : 0.0, lat.c_str(), node_lat.c_str(),
                        isp_stats.c_str(), snd_stats.c_str());
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
            print_sys_stats();
#endif
        }
        if (!run_forever && t0_us.load(std::memory_order_relaxed) > 0 &&
            esp_timer_get_time() >= stop_at)
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Join the TX thread before close(): it may still be packetizing an AU. */
    stop.store(true, std::memory_order_release);
    tx_thread.join();
    mplane.stop();
    src->close();

    drain_pay_to_sender(pay_c, snd_s);
    const int64_t  t0 = t0_us.load(std::memory_order_relaxed);
    const double   sec = t0 > 0 ? (esp_timer_get_time() - t0) / 1e6 : 0;
    const uint64_t f = frames.load(std::memory_order_relaxed);
    const uint64_t k = keys.load(std::memory_order_relaxed);
    const uint64_t b = bytes.load(std::memory_order_relaxed);
    std::string    isp_stats;
    std::string    snd_stats;
    src->query("stats", &isp_stats);
    snd->query("stats", &snd_stats);
    std::printf("live TX done: %" PRIu64 " AUs, %" PRIu64 " IDR, %.1f fps, %.0f kbps; isp %s; snd %s\n",
                f, k, sec > 0 ? f / sec : 0.0, sec > 0 ? b * 8 / sec / 1000 : 0.0,
                isp_stats.c_str(), snd_stats.c_str());
    snd->close();
    pay->close();
    if (nullptr != pm_lock)
    {
        esp_pm_lock_release(pm_lock);
        esp_pm_lock_delete(pm_lock);
    }
    print_heap("live end");
    const bool ok = run_forever ? f > 0 : (sec > 0 && f / sec >= 15);
    std::printf("live TX %s\n", ok ? "PASSED" : "FAILED");
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
    tx->configure("max_kbps", "20000");
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
#if defined(ENABLE_ESP32P4_CSI_CAMERA_SRC) && defined(ENABLE_H264_ENCODER_ESP32P4) && \
    CONFIG_VSTREAMER_IDF_LIVE_TX
        while (true)
        {
            (void)run_camera_stream();
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
