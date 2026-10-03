#include "apps/common/tx/tx_metrics.hpp"

#include "apps/stream_sdl_test/pipeline_state.hpp"

#include <cstdlib>
#include <mutex>
#include <string>

#include "core/metrics.hpp"
#include "core/stream_telemetry.hpp"

namespace vstreamer::apps::tx
{

using namespace vstreamer;
using test_app::g_pipeline_metrics;
using test_app::g_tx;

namespace
{

constexpr int k_receiver_loss_avg_samples = 5;

struct receiver_loss_tracker
{
    stream_link_counters prev {};
    bool                 have_prev = false;
    double               udp_loss[k_receiver_loss_avg_samples] {};
    double               fec_loss[k_receiver_loss_avg_samples] {};
    int                  udp_n = 0;
    int                  fec_n = 0;
    double               loss_udp_pct = 0.;
    double               loss_fec_pct = 0.;
};

receiver_loss_tracker g_sender_peer_loss;
std::mutex            g_sender_peer_loss_mu;

double interval_loss_pct(uint64_t pkts_now, uint64_t gap_now, uint64_t pkts_prev,
                         uint64_t gap_prev)
{
    if (pkts_now < pkts_prev || gap_now < gap_prev)
    {
        return -1.;
    }
    const uint64_t dp = pkts_now - pkts_prev;
    const uint64_t dg = gap_now - gap_prev;
    const uint64_t denom = dp + dg;
    if (denom < 8)
    {
        return -1.;
    }
    return static_cast<double>(dg) / static_cast<double>(denom) * 100.0;
}

void push_loss_sample(double sample, double *buf, int &n, double &avg_out)
{
    if (sample < 0.)
    {
        return;
    }
    if (n < k_receiver_loss_avg_samples)
    {
        buf[n] = sample;
        ++n;
    }
    else
    {
        for (int i = 1; i < k_receiver_loss_avg_samples; ++i)
        {
            buf[i - 1] = buf[i];
        }
        buf[k_receiver_loss_avg_samples - 1] = sample;
    }
    double sum = 0.;
    for (int i = 0; i < n; ++i)
    {
        sum += buf[i];
    }
    avg_out = sum / static_cast<double>(n);
}

void update_receiver_loss_deltas(const stream_link_counters &cur, receiver_loss_tracker &tr)
{
    if (tr.have_prev)
    {
        const double wire_inst =
            interval_loss_pct(cur.udp_packet_received, cur.udp_gap_count,
                              tr.prev.udp_packet_received, tr.prev.udp_gap_count);
        push_loss_sample(wire_inst, tr.udp_loss, tr.udp_n, tr.loss_udp_pct);

        const double fec_inst =
            interval_loss_pct(cur.fec_packet_received, cur.fec_gap_count, tr.prev.fec_packet_received,
                              tr.prev.fec_gap_count);
        push_loss_sample(fec_inst, tr.fec_loss, tr.fec_n, tr.loss_fec_pct);
    }
    tr.prev = cur;
    tr.have_prev = true;
}

}  // namespace

bool query_source_metric_string(component_source *src, const char *key, std::string &out)
{
    if (nullptr == src || nullptr == key)
    {
        return false;
    }
    std::string v;
    if (src->query(std::string_view(key), &v) != 0 || v.empty())
    {
        return false;
    }
    out = std::move(v);
    return true;
}

void store_source_pipeline_metrics(double source_out_fps, double source_out_kbps, const char *ts,
                                   component *jdec)
{
    std::string device = "noise";
    std::string media_type = "mjpeg";
    std::string pixel_type = "mjpeg";
    int         width = 0;
    int         height = 0;

    if (nullptr != g_tx.metrics_source)
    {
        (void)query_source_metric_string(g_tx.metrics_source, "device", device);
        (void)query_source_metric_string(g_tx.metrics_source, "media_type", media_type);
        (void)query_source_metric_string(g_tx.metrics_source, "pixel_type", pixel_type);
        std::string w;
        std::string h;
        if (query_source_metric_string(g_tx.metrics_source, "width", w))
        {
            width = std::atoi(w.c_str());
        }
        if (query_source_metric_string(g_tx.metrics_source, "height", h))
        {
            height = std::atoi(h.c_str());
        }
    }
    if (nullptr != jdec && (media_type == "mjpeg" || pixel_type == "mjpeg"))
    {
        std::string decoded;
        if (jdec->query("decoded_pix_fmt", &decoded) == 0 && !decoded.empty())
        {
            pixel_type = std::move(decoded);
        }
    }

    metric_store(*g_pipeline_metrics.get_metric("source.device"), device);
    metric_store(*g_pipeline_metrics.get_metric("source.width"), static_cast<int64_t>(width));
    metric_store(*g_pipeline_metrics.get_metric("source.height"), static_cast<int64_t>(height));
    metric_store(*g_pipeline_metrics.get_metric("source.pixel_type"), pixel_type);
    metric_store(*g_pipeline_metrics.get_metric("source.media_type"), media_type);
    metric_store(*g_pipeline_metrics.get_metric("source.out_fps"), source_out_fps);
    metric_store(*g_pipeline_metrics.get_metric("source.out_kbps"), source_out_kbps);
    metric_store(*g_pipeline_metrics.get_metric("source.ts"), ts);
    std::string src_state = "running";
    if (nullptr != g_tx.metrics_source)
    {
        (void)query_source_metric_string(g_tx.metrics_source, "state", src_state);
        std::string rnd;
        if (query_source_metric_string(g_tx.metrics_source, "noise-bandwidth", rnd))
        {
            metric_store(*g_pipeline_metrics.get_metric("source.noise_bandwidth"), rnd);
        }
        std::string eff_blk;
        if (query_source_metric_string(g_tx.metrics_source, "noise-luma-block-size", eff_blk))
        {
            metric_store(*g_pipeline_metrics.get_metric("source.noise_luma_block_size"), eff_blk);
        }
    }
    metric_store(*g_pipeline_metrics.get_metric("source.state"), src_state);
}

void store_sender_peer_link_metrics(uint64_t udp_recv, uint64_t fec_recv, uint64_t udp_gap,
                                    uint64_t fec_gap, double loss_udp_pct, double loss_fec_pct)
{
    metric_store(
        *g_pipeline_metrics.get_metric("stream_sender.peer_udp_packet_received"), udp_recv);
    metric_store(
        *g_pipeline_metrics.get_metric("stream_sender.peer_fec_packet_received"), fec_recv);
    metric_store(*g_pipeline_metrics.get_metric("stream_sender.peer_udp_gap_count"), udp_gap);
    metric_store(*g_pipeline_metrics.get_metric("stream_sender.peer_fec_gap_count"), fec_gap);
    metric_store(*g_pipeline_metrics.get_metric("stream_sender.peer_loss_udp_pct"), loss_udp_pct);
    metric_store(*g_pipeline_metrics.get_metric("stream_sender.peer_loss_fec_pct"), loss_fec_pct);
}

void sync_sender_peer_link_metrics_live(stream_sender &sender)
{
    const stream_peer_link snap = sender.peer_link_snapshot();
    if (!snap.have)
    {
        return;
    }
    const stream_link_counters cur = snap.report.counters;

    double loss_udp = 0.;
    double loss_fec = 0.;
    {
        std::lock_guard<std::mutex> lock(g_sender_peer_loss_mu);
        update_receiver_loss_deltas(cur, g_sender_peer_loss);
        loss_udp = g_sender_peer_loss.loss_udp_pct;
        loss_fec = g_sender_peer_loss.loss_fec_pct;
    }

    store_sender_peer_link_metrics(cur.udp_packet_received, cur.fec_packet_received,
                                   cur.udp_gap_count, cur.fec_gap_count, loss_udp, loss_fec);
    metric_store(*g_pipeline_metrics.get_metric("stream_sender.peer_report_age_ms"),
                 static_cast<double>(snap.age_ms));
    metric_store(*g_pipeline_metrics.get_metric("stream_sender.peer_reports_received"),
                 snap.reports_received);
    metric_store(*g_pipeline_metrics.get_metric("stream_sender.peer_reports_lost"),
                 snap.reports_lost);
    metric_store(*g_pipeline_metrics.get_metric("stream_sender.peer_reports_rejected"),
                 snap.reports_rejected);
}

void sync_tx_cumulative_counters(const test_app::bench_diag &d, stream_sender *sender)
{
    if (nullptr == sender)
    {
        return;
    }
    metric_store(*g_pipeline_metrics.get_metric("source.out_bytes"), d.tx_source_bytes);
    metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.in_frames"), d.tx_noise);
    metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.out_frames"), d.tx_jpeg_nv12);
    metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.out_bytes"), d.tx_jpeg_nv12_bytes);
    metric_store(*g_pipeline_metrics.get_metric("jpeg_decoder.dropped_frames"), d.tx_mjpeg_q_drop);
    metric_store(*g_pipeline_metrics.get_metric("encoder_queue.in_frames"), d.tx_jpeg_nv12);
    metric_store(*g_pipeline_metrics.get_metric("encoder_queue.out_frames"), d.tx_enc_nv12_popped);
    metric_store(*g_pipeline_metrics.get_metric("h264_encoder.in_frames"), d.tx_nv12);
    metric_store(*g_pipeline_metrics.get_metric("stream_sender.in_packets"),
                 sender->wire_pkts_sent_counter());
    metric_store(*g_pipeline_metrics.get_metric("stream_sender.in_bytes"),
                 sender->wire_bytes_sent_counter());
}

int query_encoder_qp(component_coder &enc)
{
    std::string val;
    if (enc.query("qp", &val) < 0 || val.empty())
    {
        return -1;
    }
    return std::atoi(val.c_str());
}

int query_encoder_cbr_bps(component_coder &enc)
{
    std::string val;
    if (enc.query("cbr", &val) < 0 || val.empty())
    {
        return -1;
    }
    return std::atoi(val.c_str());
}

}  // namespace vstreamer::apps::tx
