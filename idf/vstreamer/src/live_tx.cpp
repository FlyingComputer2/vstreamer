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
#include "core/thread_spawn.hpp"
#include "core/time_util.hpp"

#include "esp_heap_caps.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
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

/* Live TX: vs_isp_tx runs the RS FEC encode (~10 ms of compute per block) on core 0, so the
 * send thread sits on core 1; sharing core 0 starved it and spread FEC blocks left in lumps.
 * The mplane is on core 1 so it answers under load. Measured and not worth it (2026-10-17):
 * capture on core 1 (no change) and lwIP's tiT pinned to core 1 (vs_isp_tx +3% of core 0). */
constexpr vstreamer::thread_opts k_live_tx_thread {"vs_isp_tx", 16 * 1024, 0, 13};
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

    /* Runtime control (FEC k/n, bitrate, ...) for bench sweeps. */
    live_mplane mplane;
    if (cfg.mplane_port > 0)
    {
        mplane.add_node("src", src.get());
        mplane.add_node("pay", pay.get());
        mplane.add_node("snd", snd.get());
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        mplane.add_query("sys", "stats", [] {
            static cpu_window w;
            return sys_stats_line(&w);
        });
#endif
        if (const int mr = mplane.start(cfg.mplane_port, k_live_mplane_thread); mr < 0)
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
        vstreamer::spawn_thread(k_live_tx_thread, tx_fn);

    while (!stop.load(std::memory_order_acquire) && esp_timer_get_time() < deadline)
    {
        if (cfg.stats_interval_s > 0 &&
            esp_timer_get_time() - last_stats_us >= cfg.stats_interval_s * 1000000LL)
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
