/* metrics_sync.cpp — split from stream_sdl (P11-T4). */

#include "test_app/stream_sdl/metrics_sync.hpp"

#include "apps/common/app_metrics.hpp"
#include "apps/common/stage_latency.hpp"
#include "apps/common/tx/tx_metrics.hpp"
#include "apps/common/rx/rx_metrics.hpp"
#include "test_app/stream_sdl/pipeline_state.hpp"

#include <string>

#include "core/metrics.hpp"
#include "core/stream_telemetry.hpp"

namespace vstreamer::test_app
{

using namespace vstreamer;

namespace
{

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

}  // namespace

void update_pipeline_metrics(const bench_diag &d, component_coder *enc, stream_sender *sender,
                             stream_receiver *rcv, component_sink *preview, bool kmsdrm,
                             pipeline_rate_state &rate,
                             const test_app::channel_controller *channel,
                             component *jdec, bool jpeg_active,
                             component *dec)
{
    std::lock_guard<std::mutex> update_lock(g_pipeline_metrics_update_mu);
    const auto t_now = std::chrono::steady_clock::now();
    double     dt = 1.0;
    pipeline_counters prev {};
    uint64_t          prev_snd_pkts = 0;
    uint64_t          prev_snd_bytes = 0;
    uint64_t          prev_dec_dropped = 0;
    uint64_t          prev_sink_dropped = 0;
    uint64_t          prev_rcv_bytes = 0;
    uint64_t          prev_ch_bytes_in = 0;
    uint64_t          prev_ch_bytes_out = 0;
    uint64_t          prev_ch_fwd_drops = 0;
    if (rate.have_snap)
    {
        prev = rate.snap;
        prev_snd_pkts = rate.snd_pkts;
        prev_snd_bytes = rate.snd_bytes;
        prev_dec_dropped = rate.dec_dropped;
        prev_sink_dropped = rate.sink_dropped;
        prev_rcv_bytes = rate.rcv_bytes;
        prev_ch_bytes_in = rate.ch_bytes_in;
        prev_ch_bytes_out = rate.ch_bytes_out;
        prev_ch_fwd_drops = rate.ch_fwd_drops;
        dt = apps::elapsed_sec(rate.t0, t_now);
    }
    const pipeline_counters now = snapshot_counters(d);

    std::string rcv_stats;
    std::string snd_stats;
    if (nullptr != rcv)
    {
        (void)rcv->query("stats", &rcv_stats);
    }
    if (nullptr != sender)
    {
        (void)sender->query("stats", &snd_stats);
    }

    const uint64_t rcv_bytes_now = apps::parse_stats_field(rcv_stats, "bytes");
    const uint64_t snd_pkts_now = apps::parse_stats_field(snd_stats, "pkts");
    const uint64_t snd_bytes_now = apps::parse_stats_field(snd_stats, "bytes");

#if !defined(VSTREAMER_BENCH_TX_ONLY) && !defined(VSTREAMER_BENCH_RX_ONLY)
    const auto ch = (nullptr != channel) ? channel->forward_stats_snapshot()
                                         : test_app::channel_controller::forward_stats {};
#else
    const test_app::channel_controller::forward_stats ch {};
#endif
    const double noise_fps = apps::rate_per_sec(now.tx_noise, prev.tx_noise, dt);
    const double jpeg_out_fps = apps::rate_per_sec(now.tx_jpeg_nv12, prev.tx_jpeg_nv12, dt);
    const double enc_in_fps = apps::rate_per_sec(now.tx_nv12, prev.tx_nv12, dt);
    const double enc_pkt_ps = apps::rate_per_sec(now.tx_rtp_sock, prev.tx_rtp_sock, dt);
    const double snd_pkt_ps = apps::rate_per_sec(snd_pkts_now, prev_snd_pkts, dt);
    const double rx_pkt_ps = apps::rate_per_sec(now.rx_udp, prev.rx_udp, dt);
    const double dec_in_au_pps = apps::rate_per_sec(now.rx_dec_in_ok, prev.rx_dec_in_ok, dt);
    const double depay_au_pps = apps::rate_per_sec(now.rx_depay_au, prev.rx_depay_au, dt);
    const double dec_out_fps = apps::rate_per_sec(now.rx_nv12_out, prev.rx_nv12_out, dt);
    const double present_fps = apps::rate_per_sec(now.rx_present_ok, prev.rx_present_ok, dt);
    const uint64_t sink_dropped_now = d.rx_present_q_drop.load();
    const double   sink_drop_fps = apps::rate_per_sec(sink_dropped_now, prev_sink_dropped, dt);
    const double nv12_gap_fps =
        jpeg_out_fps > enc_in_fps ? jpeg_out_fps - enc_in_fps : 0.0;

    const int qp = (nullptr != enc) ? query_encoder_qp(*enc) : -1;
    const int qp_val = qp >= 0 ? qp : 0;

    const uint64_t nv12_q_drop = d.tx_nv12_q_drop.load();
    const uint64_t enc_input_miss = d.tx_enc_input_miss.load();
    const uint64_t enc_dropped_frames_total =
        nv12_q_drop + enc_input_miss + d.tx_enc_in_err.load();
    const uint64_t dec_dropped_now =
        d.rx_dec_in_err.load() + d.rx_depay_err.load() + d.rx_au_q_drop.load();
    const double   dec_drop_pps = apps::rate_per_sec(dec_dropped_now, prev_dec_dropped, dt);

    /* Encoder output bitrate (H.264 AU bytes); same unit as h264_encoder.cbr_kbps. */
    const uint64_t enc_out_bytes_now =
        d.tx_enc_out_bytes.load(std::memory_order_relaxed);
    double rcv_out_kbps = 0.0;
    if (rate.have_snap && dt > 0.0 && rcv_bytes_now >= prev_rcv_bytes && rcv_bytes_now > prev_rcv_bytes)
    {
        rcv_out_kbps =
            static_cast<double>(rcv_bytes_now - prev_rcv_bytes) * 8.0 / dt / 1000.0;
    }
    double snd_in_kbps = 0.0;
    if (rate.have_snap && dt > 0.0 && snd_bytes_now >= prev_snd_bytes && snd_bytes_now > prev_snd_bytes)
    {
        snd_in_kbps = static_cast<double>(snd_bytes_now - prev_snd_bytes) * 8.0 / dt / 1000.0;
    }
    double source_out_kbps = 0.0;
    if (rate.have_snap && dt > 0.0 && now.tx_source_bytes >= prev.tx_source_bytes &&
        now.tx_source_bytes > prev.tx_source_bytes)
    {
        source_out_kbps =
            static_cast<double>(now.tx_source_bytes - prev.tx_source_bytes) * 8.0 / dt / 1000.0;
    }
    double jpeg_nv12_out_kbps = 0.0;
    if (rate.have_snap && dt > 0.0 && now.tx_jpeg_nv12_bytes >= prev.tx_jpeg_nv12_bytes &&
        now.tx_jpeg_nv12_bytes > prev.tx_jpeg_nv12_bytes)
    {
        jpeg_nv12_out_kbps =
            static_cast<double>(now.tx_jpeg_nv12_bytes - prev.tx_jpeg_nv12_bytes) * 8.0 / dt /
            1000.0;
    }
    double dec_in_kbps = 0.0;
    if (rate.have_snap && dt > 0.0 && now.rx_dec_in_bytes >= prev.rx_dec_in_bytes &&
        now.rx_dec_in_bytes > prev.rx_dec_in_bytes)
    {
        dec_in_kbps =
            static_cast<double>(now.rx_dec_in_bytes - prev.rx_dec_in_bytes) * 8.0 / dt / 1000.0;
    }
    double dec_nv12_out_kbps = 0.0;
    if (rate.have_snap && dt > 0.0 && now.rx_nv12_out_bytes >= prev.rx_nv12_out_bytes &&
        now.rx_nv12_out_bytes > prev.rx_nv12_out_bytes)
    {
        dec_nv12_out_kbps =
            static_cast<double>(now.rx_nv12_out_bytes - prev.rx_nv12_out_bytes) * 8.0 / dt / 1000.0;
    }
    const double enc_q_pop_fps =
        apps::rate_per_sec(now.tx_enc_nv12_popped, prev.tx_enc_nv12_popped, dt);
    const size_t enc_q_depth = (nullptr != g_tx.metrics_nv12_q) ? g_tx.metrics_nv12_q->size() : 0;
    double       enc_q_latency_ms = 0.0;
    if (jpeg_out_fps > 0.5)
    {
        enc_q_latency_ms = static_cast<double>(enc_q_depth) * 1000.0 / jpeg_out_fps;
    }
    double       ch_fwd_kbps = 0.0;
    uint64_t     ch_fwd_drops = 0;
    double       ch_drop_pps = 0.0;
    double       ch_drop_kbps = 0.0;
#if !defined(VSTREAMER_BENCH_TX_ONLY) && !defined(VSTREAMER_BENCH_RX_ONLY)
    ch_fwd_kbps =
        ch.bytes_out > prev_ch_bytes_out
            ? static_cast<double>(ch.bytes_out - prev_ch_bytes_out) * 8.0 / dt / 1000.0
            : 0.0;
    ch_fwd_drops = ch.dropped_rate + ch.dropped_loss + ch.dropped_queue;
    ch_drop_pps = apps::rate_per_sec(ch_fwd_drops, prev_ch_fwd_drops, dt);
    if (rate.have_snap && dt > 0.0 && ch.bytes_in >= prev_ch_bytes_in &&
        ch.bytes_out >= prev_ch_bytes_out)
    {
        const uint64_t din = ch.bytes_in - prev_ch_bytes_in;
        const uint64_t dout = ch.bytes_out - prev_ch_bytes_out;
        if (din > dout)
        {
            ch_drop_kbps = static_cast<double>(din - dout) * 8.0 / dt / 1000.0;
        }
    }
#endif

    uint64_t sink_frames = now.rx_present_ok;
    std::string sink_stats;
    if (nullptr != preview && preview->query("stats", &sink_stats) == 0)
    {
        sink_frames = apps::parse_stats_field(sink_stats, "frames");
    }

    char ts[40];
    apps::format_stats_timestamp(ts, sizeof(ts));

    const bool pipeline_flowing =
        (now.tx_rtp_sock > 0 && now.rx_udp > 0) || (enc_pkt_ps > 0.5 && rx_pkt_ps > 0.5);

    const double snd_pps = snd_pkt_ps > 0.0 ? snd_pkt_ps : enc_pkt_ps;

    const double glass_ms = apps::g_glass_latency_ms.load(std::memory_order_relaxed);
    if (nullptr != channel)
    {
        metric_store(*g_pipeline_metrics.get_metric("stream_sdl.status"),
                     "running");
        metric_store(*g_pipeline_metrics.get_metric("stream_sdl.display"),
                     kmsdrm ? "kmsdrm" : "sdl");
        if (kmsdrm)
        {
            const char *note = present_fps > 0.5 ? "kmsdrm presenting decoded frames"
                                                 : "kmsdrm active; waiting for decode/present";
            metric_store(*g_pipeline_metrics.get_metric("stream_sdl.note"), note);
        }
        metric_store(*g_pipeline_metrics.get_metric("stream_sdl.pipeline_ok"),
                     pipeline_flowing ? "yes" : "warming");
        metric_store(*g_pipeline_metrics.get_metric("stream_sdl.glass_latency_ms"),
                     glass_ms);
    }
    metric_store(*g_pipeline_metrics.get_metric("latency.glass_ms"), glass_ms);
    metric_store(*g_pipeline_metrics.get_metric("latency.source_ms"),
                 apps::g_latency_source_ms.load(std::memory_order_relaxed));
    metric_store(*g_pipeline_metrics.get_metric("latency.jpeg_ms"),
                 apps::g_latency_jpeg_ms.load(std::memory_order_relaxed));
    metric_store(*g_pipeline_metrics.get_metric("latency.enc_in_ms"),
                 apps::g_latency_enc_in_ms.load(std::memory_order_relaxed));
    metric_store(*g_pipeline_metrics.get_metric("latency.enc_out_ms"),
                 apps::g_latency_enc_out_ms.load(std::memory_order_relaxed));
    metric_store(*g_pipeline_metrics.get_metric("latency.depay_ms"),
                 apps::g_latency_depay_ms.load(std::memory_order_relaxed));
    metric_store(*g_pipeline_metrics.get_metric("latency.dec_in_ms"),
                 apps::g_latency_dec_in_ms.load(std::memory_order_relaxed));
    metric_store(*g_pipeline_metrics.get_metric("latency.dec_out_ms"),
                 apps::g_latency_dec_out_ms.load(std::memory_order_relaxed));
    metric_store(*g_pipeline_metrics.get_metric("latency.present_ms"),
                 apps::g_latency_present_ms.load(std::memory_order_relaxed));

    if (nullptr != sender)
    {
        store_source_pipeline_metrics(noise_fps, source_out_kbps, ts,
                                      jpeg_active ? jdec : nullptr);
    }

    if (jpeg_active && nullptr != sender)
    {
        metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.status"), "active");
        metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.in_fps"), noise_fps);
        metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.out_fps"),
                     jpeg_out_fps);
        metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.out_kbps"),
                     jpeg_nv12_out_kbps);
        const uint64_t mjpeg_q_drop = d.tx_mjpeg_q_drop.load();
        metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.dropped_fps"),
                     apps::rate_per_sec(mjpeg_q_drop, prev.tx_mjpeg_q_drop, dt));
        metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.latency_ms"),
                     apps::g_latency_jpeg_ms.load(std::memory_order_relaxed));
        if (nullptr != jdec)
        {
            std::string sz;
            if (jdec->query("size", &sz) == 0)
            {
                metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.size"),
                             sz);
            }
            std::string workers;
            if (jdec->query("workers", &workers) == 0)
            {
                metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.workers"),
                             workers);
            }
        }
    }
    else
    {
        metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.status"), "bypass");
        metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.out_fps"), 0.0);
    }

    const uint64_t fec_recovered =
        nullptr != rcv ? query_u64(*rcv, "fec_recovered") : 0;
    const uint64_t fec_failures = nullptr != rcv ? query_u64(*rcv, "fec_failures") : 0;

    if (nullptr != sender)
    {
        metric_store(*g_pipeline_metrics.get_metric("encoder_queue.in_fps"), jpeg_out_fps);
        metric_store(*g_pipeline_metrics.get_metric("encoder_queue.out_fps"), enc_q_pop_fps);
        metric_store(*g_pipeline_metrics.get_metric("encoder_queue.size"),
                     static_cast<int64_t>(enc_q_depth));
        metric_store(*g_pipeline_metrics.get_metric("encoder_queue.latency_ms"),
                     enc_q_latency_ms);

        metric_store(*g_pipeline_metrics.get_metric("h264_encoder.in_fps"), enc_in_fps);
        metric_store(*g_pipeline_metrics.get_metric("h264_encoder.dropped_fps"),
                     nv12_gap_fps);
        metric_store(*g_pipeline_metrics.get_metric("h264_encoder.dropped_frames"),
                     enc_dropped_frames_total);
        metric_store(*g_pipeline_metrics.get_metric("h264_encoder.out_pps"), enc_pkt_ps);
        metric_store(*g_pipeline_metrics.get_metric("h264_encoder.qp"),
                     static_cast<int64_t>(qp_val));
        if (nullptr != enc)
        {
            metric_store(*g_pipeline_metrics.get_metric("h264_encoder.latency_ms"),
                         query_component_latency_ms(*enc));
        }

        metric_store(*g_pipeline_metrics.get_metric("stream_sender.in_pps"), snd_pps);
        metric_store(*g_pipeline_metrics.get_metric("stream_sender.in_kbps"), snd_in_kbps);
        metric_store(*g_pipeline_metrics.get_metric("stream_sender.fec_k"),
                     static_cast<int64_t>(query_u64(*sender, "fec_k")));
        metric_store(*g_pipeline_metrics.get_metric("stream_sender.fec_n"),
                     static_cast<int64_t>(query_u64(*sender, "fec_n")));
        metric_store(*g_pipeline_metrics.get_metric("stream_sender.fec_recovered"),
                     fec_recovered);
        metric_store(*g_pipeline_metrics.get_metric("stream_sender.fec_failures"),
                     fec_failures);
        metric_store(*g_pipeline_metrics.get_metric("stream_sender.fec_oversized"),
                     query_u64(*sender, "fec_oversized"));
    }

#if !defined(VSTREAMER_BENCH_TX_ONLY) && !defined(VSTREAMER_BENCH_RX_ONLY)
    if (nullptr != channel)
    {
        metric_store(*g_pipeline_metrics.get_metric("channel.forward_kbps"), ch_fwd_kbps);
        metric_store(*g_pipeline_metrics.get_metric("channel.dropped_pps"), ch_drop_pps);
        metric_store(*g_pipeline_metrics.get_metric("channel.dropped_kbps"), ch_drop_kbps);
        const double ch_max_kbps = channel->max_kbps();
        const double ch_constant_loss_pct = channel->constant_loss();
        metric_store(*g_pipeline_metrics.get_metric("channel.max_kbps"), ch_max_kbps);
        metric_store(*g_pipeline_metrics.get_metric("channel.constant_loss_pct"),
                     ch_constant_loss_pct);
        const size_t ch_queue = channel->forward_queue_size();
        metric_store(*g_pipeline_metrics.get_metric("channel.queue"),
                     static_cast<double>(ch_queue));
    }
#endif

    if (nullptr != rcv)
    {
        metric_store(*g_pipeline_metrics.get_metric("stream_receiver.in_pps"), rx_pkt_ps);
        metric_store(*g_pipeline_metrics.get_metric("stream_receiver.out_kbps"), rcv_out_kbps);
        metric_store(*g_pipeline_metrics.get_metric("stream_receiver.fec_recovered"),
                     fec_recovered);
        metric_store(*g_pipeline_metrics.get_metric("stream_receiver.fec_failures"),
                     fec_failures);
        metric_store(*g_pipeline_metrics.get_metric("stream_receiver.fec_hdr_errors"),
                     query_u64(*rcv, "fec_hdr_errors"));
        metric_store(*g_pipeline_metrics.get_metric("stream_receiver.fec_kn_mismatch"),
                     query_u64(*rcv, "fec_kn_mismatch"));
        metric_store(*g_pipeline_metrics.get_metric("stream_receiver.fec_evicted_blocks"),
                     query_u64(*rcv, "fec_evicted_blocks"));
        metric_store(*g_pipeline_metrics.get_metric("stream_receiver.fec_rs_failures"),
                     query_u64(*rcv, "fec_rs_failures"));
        metric_store(*g_pipeline_metrics.get_metric("stream_receiver.fec_missing_shards"),
                     query_u64(*rcv, "fec_missing_shards"));
        metric_store(*g_pipeline_metrics.get_metric("stream_receiver.fec_late_blocks"),
                     query_u64(*rcv, "fec_late_blocks"));
    }

    if (nullptr != dec || nullptr != rcv)
    {
        metric_store(*g_pipeline_metrics.get_metric("h264_decoder.in_pps"),
                     dec_in_au_pps);
        metric_store(*g_pipeline_metrics.get_metric("h264_decoder.in_kbps"), dec_in_kbps);
        metric_store(*g_pipeline_metrics.get_metric("h264_decoder.depay_au_pps"),
                     depay_au_pps);
        metric_store(*g_pipeline_metrics.get_metric("h264_decoder.dropped_pps"),
                     dec_drop_pps);
        metric_store(*g_pipeline_metrics.get_metric("h264_decoder.out_fps"), dec_out_fps);
        metric_store(*g_pipeline_metrics.get_metric("h264_decoder.out_kbps"),
                     dec_nv12_out_kbps);
        if (nullptr != dec)
        {
            metric_store(*g_pipeline_metrics.get_metric("h264_decoder.latency_ms"),
                         query_component_latency_ms(*dec));
        }
        else
        {
            metric_store(*g_pipeline_metrics.get_metric("h264_decoder.latency_ms"),
                         apps::g_latency_dec_out_ms.load(std::memory_order_relaxed));
        }
    }

    if (nullptr != preview)
    {
        metric_store(*g_pipeline_metrics.get_metric("sdl_sink.in_fps"), present_fps);
        metric_store(*g_pipeline_metrics.get_metric("sdl_sink.in_frames"), sink_frames);
        metric_store(*g_pipeline_metrics.get_metric("sdl_sink.dropped_fps"),
                     sink_drop_fps);
        metric_store(*g_pipeline_metrics.get_metric("sdl_sink.render_fps"), present_fps);
        metric_store(*g_pipeline_metrics.get_metric("sdl_sink.latency_ms"), glass_ms);
    }

    rate.snap = now;
    rate.t0 = t_now;
    rate.snd_pkts = snd_pkts_now;
    rate.snd_bytes = snd_bytes_now;
    rate.enc_out_bytes = enc_out_bytes_now;
    rate.dec_dropped = dec_dropped_now;
    rate.sink_dropped = sink_dropped_now;
    rate.rcv_bytes = rcv_bytes_now;
    rate.ch_bytes_in = ch.bytes_in;
    rate.ch_bytes_out = ch.bytes_out;
    rate.ch_pkts_out = ch.pkts_out;
    rate.ch_fwd_drops = ch_fwd_drops;
    rate.have_snap = true;
}

namespace
{

void sync_cumulative_pipeline_counters(const bench_diag &d, stream_sender *sender,
                                       const test_app::channel_controller *channel,
                                       stream_receiver *rcv)
{
#if defined(VSTREAMER_BENCH_TX_ONLY) || defined(VSTREAMER_BENCH_RX_ONLY)
    (void)channel;
#endif
    apps::tx::sync_tx_cumulative_counters(d, sender);
#if !defined(VSTREAMER_BENCH_TX_ONLY) && !defined(VSTREAMER_BENCH_RX_ONLY)
    if (nullptr != channel)
    {
        metric_store(*g_pipeline_metrics.get_metric("channel.forward_bytes"),
                     channel->forward_bytes_out_counter());
        metric_store(*g_pipeline_metrics.get_metric("channel.queue"),
                     static_cast<double>(channel->forward_queue_size()));
    }
#endif
    apps::rx::sync_rx_cumulative_counters(d, rcv);
}

struct metrics_serve_rate_state
{
    std::mutex                              mu;
    uint64_t                                prev_enc_out_bytes = 0;
    std::chrono::steady_clock::time_point   prev_t {};
    bool                                    have = false;
};

metrics_serve_rate_state g_metrics_serve_rate;

}  // namespace

void sync_pipeline_metrics_live(const bench_diag &d, stream_sender *sender, component_coder *enc,
                                stream_receiver *rcv,
                                const test_app::channel_controller *channel)
{
    const auto   t_now = std::chrono::steady_clock::now();
    const uint64_t enc_out_bytes = d.tx_enc_out_bytes.load(std::memory_order_relaxed);
    double       enc_out_kbps = 0.0;
    {
        std::lock_guard<std::mutex> lock(g_metrics_serve_rate.mu);
        if (g_metrics_serve_rate.have)
        {
            const double sec = apps::elapsed_sec(g_metrics_serve_rate.prev_t, t_now);
            if (sec > 0.0 && enc_out_bytes >= g_metrics_serve_rate.prev_enc_out_bytes &&
                enc_out_bytes > g_metrics_serve_rate.prev_enc_out_bytes)
            {
                enc_out_kbps = static_cast<double>(enc_out_bytes -
                                                   g_metrics_serve_rate.prev_enc_out_bytes) *
                               8.0 / sec / 1000.0;
            }
        }
        g_metrics_serve_rate.prev_enc_out_bytes = enc_out_bytes;
        g_metrics_serve_rate.prev_t = t_now;
        g_metrics_serve_rate.have = true;
    }
    if (nullptr != sender)
    {
        metric_store(*g_pipeline_metrics.get_metric("h264_encoder.out_bytes"),
                     d.tx_enc_out_bytes);
        metric_store(*g_pipeline_metrics.get_metric("h264_encoder.out_packets"),
                     d.tx_rtp_sock);
        metric_store(*g_pipeline_metrics.get_metric("h264_encoder.out_kbps"),
                     enc_out_kbps);
        if (nullptr != enc)
        {
            const int cbr_bps = query_encoder_cbr_bps(*enc);
            if (cbr_bps >= 0)
            {
                metric_store(*g_pipeline_metrics.get_metric("h264_encoder.cbr_kbps"),
                             static_cast<uint64_t>((cbr_bps + 500) / 1000));
            }
        }
    }
    sync_cumulative_pipeline_counters(d, sender, channel, rcv);
    if (nullptr != rcv)
    {
        apps::rx::sync_peer_link_metrics_live(*rcv);
    }
    else if (nullptr != sender)
    {
        apps::tx::sync_sender_peer_link_metrics_live(*sender);
    }
}

}  // namespace vstreamer::test_app
