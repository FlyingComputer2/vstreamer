/* diag.cpp — split from stream_sdl (P11-T4). */

#include "test_app/stream_sdl/diag.hpp"

#include "test_app/stream_sdl/channel_controller.hpp"
#include "test_app/stream_sdl/channel_ports.hpp"
#include "test_app/stream_sdl/metrics_sync.hpp"
#include "test_app/stream_sdl/pipeline_state.hpp"

#include <cinttypes>
#include <cmath>
#include <ctime>

#include "core/data_packet.hpp"
#include "core/time_util.hpp"

namespace vstreamer::test_app
{

using namespace vstreamer;

void note_source_pts(const data_packet &pkt)
{
    if (pkt.get_type() != packet_kind_e::FRAME)
    {
        return;
    }
    const frame_data &f = data_packet::cast<frame_data>(pkt);
    g_latest_source_pts.store(f.pts, std::memory_order_relaxed);
}

[[nodiscard]] size_t packet_frame_bytes(const data_packet &pkt)
{
    if (pkt.get_type() != packet_kind_e::FRAME)
    {
        return 0;
    }
    return data_packet::cast<frame_data>(pkt).buf.size();
}

[[nodiscard]] media_kind_e packet_media_kind(const data_packet &pkt)
{
    if (pkt.get_type() != packet_kind_e::FRAME)
    {
        return media_kind_e::UNKNOWN;
    }
    return data_packet::cast<frame_data>(pkt).kind;
}

[[nodiscard]] bool stage_latency_log_enabled()
{
    static const bool env_on = [] {
        const char *v = std::getenv("VSTREAMER_LOG_STAGE_LATENCY");
        return nullptr != v && v[0] != '\0' && 0 != std::strcmp(v, "0");
    }();
    return g_diag.load() || env_on;
}

[[nodiscard]] int stage_latency_log_stride()
{
    static const int every = [] {
        const char *v = std::getenv("VSTREAMER_STAGE_LATENCY_EVERY");
        if (nullptr == v || v[0] == '\0')
        {
            return 1;
        }
        char       *end = nullptr;
        const long n = std::strtol(v, &end, 10);
        return (end != v && n > 0) ? static_cast<int>(n) : 1;
    }();
    return every;
}

void record_stage_latency_ms(const char *stage, const data_packet &, double ms)
{
    if (nullptr == stage)
    {
        return;
    }
    if (0 == std::strcmp(stage, "source"))
    {
        g_latency_source_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "jpeg_nv12"))
    {
        g_latency_jpeg_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "enc_in"))
    {
        g_latency_enc_in_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "enc_out"))
    {
        g_latency_enc_out_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "depay"))
    {
        g_latency_depay_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "dec_in"))
    {
        g_latency_dec_in_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "dec_out"))
    {
        g_latency_dec_out_ms.store(ms, std::memory_order_relaxed);
    }
    else if (0 == std::strcmp(stage, "present"))
    {
        g_latency_present_ms.store(ms, std::memory_order_relaxed);
        g_glass_latency_ms.store(ms, std::memory_order_relaxed);
    }
}

void log_stage_latency(const char *stage, const data_packet &pkt)
{
    if (nullptr == stage || pkt.get_type() != packet_kind_e::FRAME)
    {
        return;
    }
    const frame_data &f = data_packet::cast<frame_data>(pkt);
    if (f.capture_mono_ns <= 0)
    {
        return;
    }
    const int64_t now_ns = steady_mono_ns();
    const double  ms = static_cast<double>(now_ns - f.capture_mono_ns) / 1e6;
    if (ms >= 0.0)
    {
        record_stage_latency_ms(stage, pkt, ms);
    }

    if (!stage_latency_log_enabled())
    {
        return;
    }
    const int stride = stage_latency_log_stride();
    if (stride > 1 && (f.pts % stride) != 0)
    {
        return;
    }
    std::fprintf(stderr, "stage_latency: %-10s %7.2f ms pts=%" PRId64 "\n", stage, ms, f.pts);
}
void log_bench_diag(const bench_diag &d, stream_receiver &rcv, stream_sender &sender,
                    h264_encoder_t &enc, const test_app::channel_controller *channel)
{
    std::string rcv_stats;
    std::string snd_stats;
    std::string enc_qp;
    (void)rcv.query("stats", &rcv_stats);
    (void)sender.query("stats", &snd_stats);
    (void)enc.query("qp", &enc_qp);

    const auto ch = (nullptr != channel) ? channel->forward_stats_snapshot()
                                         : test_app::channel_controller::forward_stats {};
    std::fprintf(stderr,
                 "diag: tx noise=%" PRIu64 " jpeg_nv12=%" PRIu64 " nv12=%" PRIu64
                 " enc_err=%" PRIu64 " rtp=%" PRIu64
                 " | rx udp=%" PRIu64 " au=%" PRIu64 " dec_in=%" PRIu64
                 "(eagain=%" PRIu64 " err=%" PRIu64 ") dec_out(eagain=%" PRIu64 " err=%" PRIu64
                 ") nv12=%" PRIu64
                 " present_ok=%" PRIu64 " present_err=%" PRIu64 " present_q_drop=%" PRIu64
                 " au_q_drop=%" PRIu64
                 " | ch in=%" PRIu64 " out=%" PRIu64 " drop_r=%" PRIu64 " drop_l=%" PRIu64
                 " drop_q=%" PRIu64
                 " | rcv{%.*s} snd{%.*s} qp=%.*s\n",
                 d.tx_noise.load(), d.tx_jpeg_nv12.load(), d.tx_nv12.load(),
                 d.tx_enc_in_err.load(), d.tx_rtp_sock.load(), d.rx_udp.load(),
                 d.rx_depay_au.load(), d.rx_dec_in_ok.load(), d.rx_dec_in_eagain.load(),
                 d.rx_dec_in_err.load(), d.rx_dec_out_eagain.load(), d.rx_dec_out_err.load(),
                 d.rx_nv12_out.load(), d.rx_present_ok.load(),
                 d.rx_present_err.load(), d.rx_present_q_drop.load(), d.rx_au_q_drop.load(),
                 ch.pkts_in, ch.pkts_out,
                 ch.dropped_rate,
                 ch.dropped_loss, ch.dropped_queue, static_cast<int>(rcv_stats.size()), rcv_stats.data(),
                 static_cast<int>(snd_stats.size()), snd_stats.data(),
                 static_cast<int>(enc_qp.size()), enc_qp.data());
}
[[nodiscard]] pipeline_counters snapshot_counters(const bench_diag &d)
{
    pipeline_counters c;
    c.tx_noise = d.tx_noise.load();
    c.tx_source_bytes = d.tx_source_bytes.load();
    c.tx_jpeg_nv12 = d.tx_jpeg_nv12.load();
    c.tx_jpeg_nv12_bytes = d.tx_jpeg_nv12_bytes.load();
    c.tx_mjpeg_q_drop = d.tx_mjpeg_q_drop.load();
    c.tx_nv12_q_drop = d.tx_nv12_q_drop.load();
    c.tx_enc_nv12_popped = d.tx_enc_nv12_popped.load();
    c.tx_nv12 = d.tx_nv12.load();
    c.tx_enc_input_miss = d.tx_enc_input_miss.load();
    c.tx_enc_out_bytes = d.tx_enc_out_bytes.load();
    c.tx_rtp_sock = d.tx_rtp_sock.load();
    c.rx_udp = d.rx_udp.load();
    c.rx_dec_in_ok = d.rx_dec_in_ok.load();
    c.rx_dec_in_bytes = d.rx_dec_in_bytes.load();
    c.rx_depay_au = d.rx_depay_au.load();
    c.rx_nv12_out = d.rx_nv12_out.load();
    c.rx_nv12_out_bytes = d.rx_nv12_out_bytes.load();
    c.rx_present_ok = d.rx_present_ok.load();
    return c;
}

[[nodiscard]] double elapsed_sec(std::chrono::steady_clock::time_point t0,
                                 std::chrono::steady_clock::time_point t1)
{
    const auto dt = std::chrono::duration_cast<std::chrono::duration<double>>(t1 - t0);
    return dt.count() > 0.0 ? dt.count() : 1.0;
}

[[nodiscard]] double rate_per_sec(uint64_t now, uint64_t prev, double dt_sec)
{
    if (now <= prev)
    {
        return 0.0;
    }
    return static_cast<double>(now - prev) / dt_sec;
}

[[nodiscard]] uint64_t parse_stats_field(std::string_view stats, const char *key)
{
    const std::string prefix = std::string(key) + "=";
    const auto              pos = stats.find(prefix);
    if (pos == std::string_view::npos)
    {
        return 0;
    }
    const char *start = stats.data() + pos + prefix.size();
    char       *end = nullptr;
    return std::strtoull(start, &end, 10);
}

template <typename Comp>
[[nodiscard]] double query_rate_kbps(const Comp &comp, const char *key)
{
    std::string v;
    if (comp.query(key, &v) != 0)
    {
        return 0.;
    }
    return std::strtod(v.c_str(), nullptr);
}

template <typename Comp>
[[nodiscard]] uint64_t query_u64(const Comp &comp, const char *key)
{
    std::string v;
    if (comp.query(key, &v) != 0)
    {
        return 0;
    }
    char *end = nullptr;
    return std::strtoull(v.c_str(), &end, 10);
}

void log_bench_rate_line(const pipeline_rate_state &rate, h264_encoder_t &enc,
                         const bench_diag &diag)
{
    if (!rate.have_snap)
    {
        return;
    }
    const uint64_t bytes = diag.tx_enc_out_bytes.load();
    const auto     t_now = std::chrono::steady_clock::now();
    const double   dt = elapsed_sec(rate.t0, t_now);
    if (dt < 0.5 || bytes <= rate.enc_out_bytes)
    {
        return;
    }
    const double enc_out_kbps =
        static_cast<double>(bytes - rate.enc_out_bytes) * 8.0 / dt / 1000.0;
    const int cbr_bps = query_encoder_cbr_bps(enc);
    const int cbr_kbps =
        cbr_bps > 0 ? (cbr_bps + 500) / 1000 : test_app::k_encoder_default_cbr_kbps;
    const int qp = query_encoder_qp(enc);
    std::fprintf(stderr, "bench_metrics: out_kbps=%.0f cbr=%d qp=%d dt=%.1f\n", enc_out_kbps,
                 cbr_kbps, qp >= 0 ? qp : 0, dt);
}
void format_stats_timestamp(char *buf, size_t buflen)
{
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t sec = clock::to_time_t(now);
    std::tm           tm_local {};
#if defined(_WIN32)
    localtime_s(&tm_local, &sec);
#else
    localtime_r(&sec, &tm_local);
#endif
    std::snprintf(buf, buflen, "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm_local.tm_year + 1900,
                  tm_local.tm_mon + 1, tm_local.tm_mday, tm_local.tm_hour, tm_local.tm_min,
                  tm_local.tm_sec, static_cast<int>(ms.count()));
}

double query_component_latency_ms(component &c)
{
    std::string v;
    if (c.query("latency_ms", &v) != 0 || v.empty())
    {
        return 0.0;
    }
    char *end = nullptr;
    const double ms = std::strtod(v.c_str(), &end);
    if (end == v.c_str() || ms < 0.0)
    {
        return 0.0;
    }
    return ms;
}

}  // namespace vstreamer::test_app
