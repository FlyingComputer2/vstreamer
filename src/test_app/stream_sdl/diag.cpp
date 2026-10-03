/* diag.cpp — split from stream_sdl (P11-T4). */

#include "test_app/stream_sdl/diag.hpp"

#include "apps/common/app_metrics.hpp"
#include "test_app/stream_sdl/channel_controller.hpp"
#include "test_app/stream_sdl/channel_ports.hpp"
#include "test_app/stream_sdl/metrics_sync.hpp"

#include <cinttypes>
#include <cmath>
#include <ctime>

#include "core/data_packet.hpp"

namespace vstreamer::test_app
{

using namespace vstreamer;

void log_bench_diag(const bench_diag &d, stream_receiver &rcv, stream_sender &sender,
                    component_coder &enc, const test_app::channel_controller *channel)
{
#if defined(VSTREAMER_BENCH_TX_ONLY) || defined(VSTREAMER_BENCH_RX_ONLY)
    (void)channel;
#endif
    std::string rcv_stats;
    std::string snd_stats;
    std::string enc_qp;
    (void)rcv.query("stats", &rcv_stats);
    (void)sender.query("stats", &snd_stats);
    (void)enc.query("qp", &enc_qp);

#if !defined(VSTREAMER_BENCH_TX_ONLY) && !defined(VSTREAMER_BENCH_RX_ONLY)
    const auto ch = (nullptr != channel) ? channel->forward_stats_snapshot()
                                         : test_app::channel_controller::forward_stats {};
#else
    const test_app::channel_controller::forward_stats ch {};
#endif
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

void log_bench_rate_line(const pipeline_rate_state &rate, component_coder &enc,
                         const bench_diag &diag)
{
    if (!rate.have_snap)
    {
        return;
    }
    const uint64_t bytes = diag.tx_enc_out_bytes.load();
    const auto     t_now = std::chrono::steady_clock::now();
    const double   dt = apps::elapsed_sec(rate.t0, t_now);
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
