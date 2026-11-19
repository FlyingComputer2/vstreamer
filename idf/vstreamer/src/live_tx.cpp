#include "core/default_reactor_host.hpp"
#include "vstreamer_idf/live_tx.hpp"

#include "vstreamer_idf/live_mplane.hpp"

#include "sdkconfig.h"

#include "core/component.hpp"
#include "core/component_coder.hpp"
#include "core/component_factory.hpp"
#include "core/component_pdu.hpp"
#include "core/component_sink.hpp"
#include "core/component_source.hpp"
#include "core/config_keys.hpp"
#include "core/thread_spawn.hpp"
#include "core/time_util.hpp"

#include "esp_heap_caps.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <sys/stat.h>

#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{

/* Thread placement on the P4's two cores. Components take it as "<role>_cpu" / "<role>_prio"
 * keys and default to any core. */
struct place_kv
{
    const char *key;
    const char *value;
};

/* Live TX: vs_isp_tx (camera, H.264, payloader) runs on core 0 and the send thread on core 1;
 * sharing core 0 starved the send thread.
 * The mplane is on core 1 so it answers under load. Measured and not worth it (2026-10-17):
 * capture on core 1 (no change) and lwIP's tiT pinned to core 1 (vs_isp_tx +3% of core 0).
 * Its stack is in PSRAM: with live video internal RAM is nearly full, and a 16 KB internal
 * request then lands in LP SRAM (slow for the CPU), which made the whole thread 6-8x slower
 * (rover 14-20 fps, vs_isp_tx 95% of core 0). Check with `get sys stack` (sys-applet-esp32p4). */
constexpr vstreamer::thread_opts k_live_tx_thread {"vs_isp_tx", 16 * 1024, 0, 13, true};
constexpr vstreamer::thread_opts k_live_mplane_thread {"vs_mplane", 6 * 1024, 1, 5};
// esp32_h264_isp_ng_src dequeues camera buffers and encodes them on the same worker.
constexpr place_kv k_live_src_place[] = {{"encode_cpu", "1"}, {"encode_prio", "13"}};
constexpr place_kv k_live_snd_place[] = {{"send_cpu", "1"}, {"send_prio", "14"}};

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

void apply_kvs(vstreamer::component &c, const char *who, const vstreamer::idf::kv_list &kvs)
{
    for (const auto &kv : kvs)
    {
        if (const int r = c.configure(kv.first, kv.second); r < 0)
        {
            std::printf("live TX: %s %s=%s rejected (%d)\n", who, kv.first.c_str(),
                        kv.second.c_str(), r);
        }
    }
}

/* ESP_PM_APB_FREQ_MAX for the lifetime of one live TX run. */
class pm_lock_guard
{
public:
    pm_lock_guard()
    {
        if (esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "live_tx", &lock) == ESP_OK)
        {
            esp_pm_lock_acquire(lock);
        }
        else
        {
            lock = nullptr;
        }
    }
    ~pm_lock_guard()
    {
        if (nullptr != lock)
        {
            esp_pm_lock_release(lock);
            esp_pm_lock_delete(lock);
        }
    }
    pm_lock_guard(const pm_lock_guard &) = delete;
    pm_lock_guard &operator=(const pm_lock_guard &) = delete;

private:
    esp_pm_lock_handle_t lock = nullptr;
};

/* TX thread time per stats interval: payloader calls, stream_sender input (queueing) and how
 * often the sender pushed back. */
std::atomic<int64_t>  g_pay_us {0};
std::atomic<int64_t>  g_snd_us {0};
std::atomic<uint64_t> g_snd_calls {0};
std::atomic<uint64_t> g_snd_eagain {0};

void drain_pay_to_sender(vstreamer::component_coder *pay, vstreamer::component_sink *sender)
{
    vstreamer::component_pdu out;
    for (;;)
    {
        const int64_t t_pay = esp_timer_get_time();
        const int     pr = pay->output(out);
        g_pay_us.fetch_add(esp_timer_get_time() - t_pay, std::memory_order_relaxed);
        if (pr != 0)
        {
            break;
        }
        while (true)
        {
            const int64_t t_snd = esp_timer_get_time();
            const int     r = sender->input(std::move(out));
            g_snd_us.fetch_add(esp_timer_get_time() - t_snd, std::memory_order_relaxed);
            g_snd_calls.fetch_add(1, std::memory_order_relaxed);
            if (0 == r)
            {
                break;
            }
            if (-EAGAIN != r)
            {
                std::printf("stream_sender input failed: %d\n", r);
                return;
            }
            g_snd_eagain.fetch_add(1, std::memory_order_relaxed);
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
        const int64_t t_pay = esp_timer_get_time();
        const int     r = pay->input(std::move(out));
        g_pay_us.fetch_add(esp_timer_get_time() - t_pay, std::memory_order_relaxed);
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

/*
 * The MKV tee after the encoder. The TX thread hands it AUs (tee(), never blocks); the
 * run_live_tx thread starts and stops recordings (poll()), since mkv_sink's configure() and
 * close() wait on card writes. The m-plane only flips the request flag and reads the cached
 * status, so it never waits on the card either (it may run on a shared reactor).
 */
class live_recorder
{
public:
    int open(const vstreamer::idf::live_tx_config &cfg, int fps)
    {
        output_ = cfg.rec_output;
        sink_ = vstreamer::component_factory::create_sink("mkv_sink");
        if (!sink_)
        {
            return -ENOTSUP;
        }
        const std::string fps_s = std::to_string(fps > 0 ? fps : 30);
        const vstreamer::idf::kv_list defaults {{"fps", fps_s},       {"queue_depth", "30"},
                                                {"mux_cpu", "1"},     {"mux_prio", "4"},
                                                {"mux_psram_stack", "1"}};
        apply_kvs(*sink_, "rec", defaults);
        apply_kvs(*sink_, "rec", cfg.rec);
        const int r = sink_->open();
        if (r < 0)
        {
            sink_.reset();
            return r;
        }
        set_status("idle");
        return 0;
    }

    [[nodiscard]] bool enabled() const { return sink_ != nullptr; }

    /* TX thread. Caps always (mkv_sink needs them before the first AU); AUs only while a
     * recording runs, copied: the encoder's AU buffers are a small pool, and holding them
     * while the card is slow would stall the encoder. */
    void tee(const vstreamer::component_pdu &in)
    {
        if (!sink_)
        {
            return;
        }
        if (in.sdu_type == vstreamer::sdu_type_e::CAPS_VIDEO_CODED)
        {
            vstreamer::component_pdu caps = in;
            (void)sink_->input(std::move(caps));
            return;
        }
        if (in.sdu_type != vstreamer::sdu_type_e::H264_AU ||
            !active_.load(std::memory_order_acquire) || in.sdu.size() == 0)
        {
            return;
        }
        const size_t n = in.sdu.size();
        auto *p = static_cast<std::byte *>(
            heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (nullptr == p)
        {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::memcpy(p, in.sdu.u8(), n);
        vstreamer::component_pdu copy;
        copy.ts_us = in.ts_us;
        copy.seq = in.seq;
        copy.sdu_type = in.sdu_type;
        copy.port = in.port;
        copy.flags = in.flags;
        copy.sdu = bfc::shared_sized_buffer::adopt(p, n, n,
                                                   [](std::byte *b) { heap_caps_free(b); });
        if (sink_->input(std::move(copy)) < 0)
        {
            /* -EAGAIN: the queue is full, the card is behind. */
            dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    /* run_live_tx thread, every loop: follows the request, refreshes the cached status. */
    void poll(bool want, vstreamer::component &src)
    {
        if (!sink_)
        {
            return;
        }
        if (want && !running_)
        {
            start(src);
        }
        else if (!want && running_)
        {
            stop();
        }
        const int64_t now = esp_timer_get_time();
        if (now - last_status_us_ >= 1000000)
        {
            last_status_us_ = now;
            refresh_status();
        }
    }

    void close()
    {
        if (!sink_)
        {
            return;
        }
        stop();
        sink_->close();
        refresh_status();
        sink_.reset();
    }

    [[nodiscard]] std::string status() const
    {
        std::lock_guard<std::mutex> lock(status_mu_);
        return status_;
    }

    [[nodiscard]] std::string file() const
    {
        std::lock_guard<std::mutex> lock(status_mu_);
        return file_;
    }

private:
    void set_status(std::string s)
    {
        std::lock_guard<std::mutex> lock(status_mu_);
        status_ = std::move(s);
    }

    /* "<stem>_NNNN<ext>" with the first NNNN whose first segment does not exist yet. */
    std::string next_template() const
    {
        std::string stem = output_;
        std::string ext = ".mkv";
        const auto slash = output_.find_last_of('/');
        const auto dot = output_.find_last_of('.');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        {
            stem = output_.substr(0, dot);
            ext = output_.substr(dot);
        }
        if (slash != std::string::npos && slash > 0)
        {
            (void)mkdir(output_.substr(0, slash).c_str(), 0777);
        }
        for (int i = 1; i <= 9999; ++i)
        {
            char idx[16];
            std::snprintf(idx, sizeof(idx), "_%04d", i);
            struct stat st {};
            if (stat((stem + idx + "-001" + ext).c_str(), &st) != 0)
            {
                return stem + idx + ext;
            }
        }
        return {};
    }

    void start(vstreamer::component &src)
    {
        running_ = true;
        const std::string tmpl = next_template();
        if (tmpl.empty() || sink_->configure("output", tmpl) < 0)
        {
            std::printf("live TX rec: no free name for %s\n", output_.c_str());
            return;
        }
        {
            std::lock_guard<std::mutex> lock(status_mu_);
            file_ = tmpl;
        }
        active_.store(true, std::memory_order_release);
        /* The file starts at a keyframe with SPS/PPS; ask for one now instead of waiting a
         * GOP. */
        (void)src.configure(vstreamer::keys::idr.id, 0);
        std::printf("live TX rec started: %s\n", tmpl.c_str());
    }

    void stop()
    {
        running_ = false;
        active_.store(false, std::memory_order_release);
        /* Write what is queued (up to ~1 s), then finish the file (cues, sizes). */
        for (int i = 0; i < 300; ++i)
        {
            std::string q;
            if (sink_->query("queued", &q) < 0 || q == "0")
            {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        std::string seg;
        (void)sink_->query("segment", &seg);
        (void)sink_->configure("output", "");
        std::printf("live TX rec stopped: %s\n", seg.c_str());
        refresh_status();
    }

    void refresh_status()
    {
        std::string stats;
        std::string seg;
        std::string werr;
        std::string skipped;
        (void)sink_->query("stats", &stats);
        (void)sink_->query("segment", &seg);
        (void)sink_->query("write_errors", &werr);
        (void)sink_->query("skipped", &skipped);
        char buf[96];
        std::snprintf(buf, sizeof(buf), " dropped=%" PRIu64 " write_errors=%s skipped=%s",
                      dropped_.load(std::memory_order_relaxed), werr.c_str(), skipped.c_str());
        std::string s = (running_ ? "on " : "off ") + stats;
        if (!seg.empty())
        {
            s += " file=" + seg;
        }
        set_status(s + buf);
    }

    std::string                               output_;
    std::unique_ptr<vstreamer::component_sink> sink_;
    std::atomic<bool>                         active_ {false};
    std::atomic<uint64_t>                     dropped_ {0};
    bool                                      running_ = false; // run_live_tx thread only
    int64_t                                   last_status_us_ = 0;
    mutable std::mutex                        status_mu_;
    std::string                               status_;
    std::string                               file_;
};

int run_live_tx_impl(const vstreamer::idf::live_tx_config &cfg)
{
    const int k_seconds = cfg.seconds;
    const bool  run_forever = (0 == k_seconds);

    const bool queue_mode = cfg.peer.empty();
    if (queue_mode && (cfg.queue_tx.empty() || cfg.queue_rx.empty() || cfg.queue_reactor.empty()))
    {
        std::printf("live TX: needs peer, or queue_tx, queue_rx and queue_reactor\n");
        return -1;
    }
    const std::string dest = queue_mode ? "queue:" + cfg.queue_tx + "," + cfg.queue_rx + "@" +
                                              cfg.queue_reactor
                                        : cfg.peer;
    /* The UDP transport runs on a reactor of its own; queues use the caller's. */
    std::optional<vstreamer::default_reactor_host> sender_reactor;
    if (!queue_mode)
    {
        sender_reactor.emplace();
    }
    auto src = vstreamer::component_factory::create_source("esp32_h264_isp_ng_src");
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
    /* Held for the whole stream and released on every return (also the early ones: callers
     * retry, and each attempt would otherwise leak a lock pinning the APB clock). */
    pm_lock_guard pm_lock;
    apply_kvs(*src, "src", cfg.src);
    apply_place(*src, k_live_src_place);

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
    apply_kvs(*pay, "pay", cfg.pay);
    const bool transport_ok =
        queue_mode ? snd->configure("reactor", cfg.queue_reactor) >= 0 &&
                         snd->configure("queue_transport_tx", cfg.queue_tx) >= 0 &&
                         snd->configure("queue_transport_rx", cfg.queue_rx) >= 0
                   : snd->configure("reactor", sender_reactor->name()) >= 0 &&
                         snd->configure("udp_transport", cfg.peer) >= 0;
    if (!transport_ok)
    {
        std::printf("live TX: bad transport '%s'\n", dest.c_str());
        return -1;
    }
    apply_kvs(*snd, "snd", cfg.snd);
    apply_place(*snd, k_live_snd_place);

    if (pay->open() < 0 || snd->open() < 0)
    {
        std::printf("live TX open failed\n");
        return -1;
    }

    live_recorder rec;
    if (!cfg.rec_output.empty())
    {
        std::string fps_s;
        (void)src->query("fps", &fps_s);
        if (const int rr = rec.open(cfg, std::atoi(fps_s.c_str())); rr < 0)
        {
            std::printf("live TX rec: mkv_sink open failed (%d)\n", rr);
        }
    }
    std::atomic<bool>  own_record {false};
    std::atomic<bool> &record_request = cfg.record ? *cfg.record : own_record;

    /* Runtime control (bitrate, gop, ...) for bench sweeps. */
    std::atomic<bool>  own_pause {false}; // outlives the mplane thread
    std::atomic<bool> &pause_request = cfg.pause ? *cfg.pause : own_pause;
    live_mplane        mplane;
    if (cfg.mplane_port > 0)
    {
        mplane.add_control(
            "tx", "pause",
            [&pause_request] { return pause_request.load() ? std::string("1") : std::string("0"); },
            [&pause_request](std::string_view v) {
                if ("1" != v && "0" != v)
                {
                    return -EINVAL;
                }
                pause_request.store("1" == v, std::memory_order_release);
                return 0;
            });
        mplane.add_control(
            "rec", "on",
            [&record_request] {
                return record_request.load() ? std::string("1") : std::string("0");
            },
            [&record_request, &rec](std::string_view v) {
                if ("1" != v && "0" != v)
                {
                    return -EINVAL;
                }
                if (!rec.enabled())
                {
                    return -ENODEV;
                }
                record_request.store("1" == v, std::memory_order_release);
                return 0;
            });
        mplane.add_query("rec", "stats", [&rec] { return rec.status(); });
        mplane.add_query("rec", "file", [&rec] { return rec.file(); });
        mplane.add_node("src", src.get());
        mplane.add_node("pay", pay.get());
        mplane.add_node("snd", snd.get());
        const int mr = cfg.mplane_reactor.empty()
                           ? mplane.start(cfg.mplane_port, k_live_mplane_thread)
                           : mplane.start(cfg.mplane_port, cfg.mplane_reactor);
        if (mr < 0)
        {
            std::printf("live TX mplane: bind :%d failed (%d)\n", cfg.mplane_port, mr);
        }
        else
        {
            std::printf("live TX mplane on udp :%d\n", cfg.mplane_port);
        }
    }

    if (run_forever)
    {
        std::printf("live TX streaming %s until reset (size %s, esp32_h264_isp_ng_src)\n",
                    dest.c_str(), size.c_str());
    }
    else
    {
        std::printf("live TX streaming %s for %d s (size %s, esp32_h264_isp_ng_src)\n",
                    dest.c_str(), k_seconds, size.c_str());
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
    int64_t       last_enc_us = last_stats_us;
    uint64_t      last_frames = 0;
    uint64_t      last_bytes = 0;

    auto tx_fn = [&]() {
        bool paused = false;
        bool wait_key = false;
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
            rec.tee(out);
            const bool pause = pause_request.load(std::memory_order_acquire);
            if (pause != paused)
            {
                paused = pause;
                std::printf("live TX %s\n", paused ? "paused" : "resumed");
                if (!paused)
                {
                    (void)src->configure(vstreamer::keys::idr.id, 0);
                    wait_key = true;
                }
            }
            if (paused)
            {
                continue;
            }
            if (wait_key)
            {
                if (!vstreamer::has_flag(out, vstreamer::pdu_flag_e::KEY))
                {
                    continue;
                }
                wait_key = false;
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
        vstreamer::spawn_thread(k_live_tx_thread, tx_fn);

    while (!stop.load(std::memory_order_acquire) && esp_timer_get_time() < deadline)
    {
        rec.poll(record_request.load(std::memory_order_acquire), *src);
        if (cfg.stats_interval_s > 0 &&
            esp_timer_get_time() - last_stats_us >= cfg.stats_interval_s * 1000000LL)
        {
            const int64_t  now_us = esp_timer_get_time();
            const double   stat_dt = (now_us - last_stats_us) / 1e6;
            last_stats_us = now_us;
            /* Snapshot before the queries below: they can wait on the encoder. fps and kbps
             * cover this interval; the totals since start are in "live TX done". */
            const uint64_t f = frames.load(std::memory_order_relaxed);
            const uint64_t b = bytes.load(std::memory_order_relaxed);
            const double   win_fps = stat_dt > 0 ? (f - last_frames) / stat_dt : 0.0;
            const double   win_kbps = stat_dt > 0 ? (b - last_bytes) * 8 / stat_dt / 1000 : 0.0;
            last_frames = f;
            last_bytes = b;
            std::string isp_stats;
            std::string snd_stats;
            std::string enc_frames_s;
            src->query("stats", &isp_stats);
            snd->query("stats", &snd_stats);
            (void)src->query("enc_frames", &enc_frames_s);
            /* Timed on its own: the query returns after the encoder lets go of its mutex. */
            const int64_t enc_us = esp_timer_get_time();
            const double  enc_dt = (enc_us - last_enc_us) / 1e6;
            last_enc_us = enc_us;
            uint64_t enc_total = 0;
            if (!enc_frames_s.empty())
            {
                enc_total = std::strtoull(enc_frames_s.c_str(), nullptr, 10);
            }
            const double enc_fps =
                enc_dt > 0 && enc_total >= last_enc_frames
                    ? static_cast<double>(enc_total - last_enc_frames) / enc_dt
                    : 0.0;
            last_enc_frames = enc_total;
            const int64_t age_n = g_age_n.exchange(0);
            const int64_t age_sum = g_age_sum_us.exchange(0);
            const int64_t age_max = g_age_max_us.exchange(0);
            const int64_t age_in = g_age_in_sum_us.exchange(0);
            std::string lat;
            std::string node_lat;
            (void)src->query("latency_ms", &lat);
            (void)src->query("node_latency_ms", &node_lat);
            const int64_t  pay_us = g_pay_us.exchange(0);
            const int64_t  snd_us = g_snd_us.exchange(0);
            const uint64_t snd_calls = g_snd_calls.exchange(0);
            const uint64_t snd_eagain = g_snd_eagain.exchange(0);
            std::printf("live TX %.1f fps (enc %.1f) %.0f kbps age_ms=%.1f/%.1f (at tx %.1f) "
                        "latency_ms=%s node_latency_ms=%s; tx pay=%.1f%% snd=%.1f%% "
                        "snd_calls=%" PRIu64 " snd_eagain=%" PRIu64 "; isp %s; snd %s%s%s\n",
                        win_fps, enc_fps, win_kbps,
                        age_n > 0 ? age_sum / 1000.0 / age_n : 0.0, age_max / 1000.0,
                        age_n > 0 ? age_in / 1000.0 / age_n : 0.0, lat.c_str(), node_lat.c_str(),
                        stat_dt > 0 ? pay_us / (stat_dt * 1e4) : 0.0,
                        stat_dt > 0 ? snd_us / (stat_dt * 1e4) : 0.0, snd_calls, snd_eagain,
                        isp_stats.c_str(), snd_stats.c_str(), rec.enabled() ? "; rec " : "",
                        rec.enabled() ? rec.status().c_str() : "");
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
    rec.close();
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
    print_heap("live end");
    const bool ok = run_forever ? f > 0 : (sec > 0 && f / sec >= 15);
    std::printf("live TX %s\n", ok ? "PASSED" : "FAILED");
    return ok ? 0 : -1;
}

}  // namespace

namespace vstreamer::idf
{

int run_live_tx(const live_tx_config &cfg)
{
    return run_live_tx_impl(cfg);
}

}  // namespace vstreamer::idf
