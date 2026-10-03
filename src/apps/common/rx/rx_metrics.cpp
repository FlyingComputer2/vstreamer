#include "apps/common/rx/rx_metrics.hpp"

#include "apps/common/stage_latency.hpp"
#include "apps/common/tx/tx_metrics.hpp"
#include "apps/stream_sdl_test/pipeline_state.hpp"

#include <chrono>
#include <mutex>
#include <thread>

#include "core/metrics.hpp"
#include "core/stream_telemetry.hpp"

namespace vstreamer::apps::rx
{

using namespace vstreamer;
using test_app::g_pipeline_metrics;
using test_app::g_run;

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

receiver_loss_tracker g_rcv_loss;
std::mutex            g_rcv_loss_mu;

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

void sync_peer_link_metrics_live(stream_receiver &rcv)
{
    const stream_link_counters cur = rcv.link_counters_snapshot();

    double loss_udp = 0.;
    double loss_fec = 0.;
    {
        std::lock_guard<std::mutex> lock(g_rcv_loss_mu);
        update_receiver_loss_deltas(cur, g_rcv_loss);
        loss_udp = g_rcv_loss.loss_udp_pct;
        loss_fec = g_rcv_loss.loss_fec_pct;
    }

    tx::store_sender_peer_link_metrics(cur.udp_packet_received, cur.fec_packet_received,
                                       cur.udp_gap_count, cur.fec_gap_count, loss_udp, loss_fec);
}

void sync_rx_cumulative_counters(const test_app::bench_diag &d, stream_receiver *rcv)
{
    if (nullptr == rcv)
    {
        return;
    }
    const uint64_t fec_pkts =
        rcv->peer_fec_packet_received_counter().load(std::memory_order_relaxed);
    const uint64_t in_pkts = fec_pkts > 0 ? fec_pkts : d.rx_udp.load(std::memory_order_relaxed);
    metric_store(*g_pipeline_metrics.get_metric("stream_receiver.in_packets"), in_pkts);
    metric_store(*g_pipeline_metrics.get_metric("stream_receiver.out_bytes"),
                 rcv->egress_payload_bytes_counter());

    metric_store(*g_pipeline_metrics.get_metric("h264_decoder.in_packets"), d.rx_dec_in_ok);
    metric_store(*g_pipeline_metrics.get_metric("h264_decoder.in_bytes"), d.rx_dec_in_bytes);
    metric_store(*g_pipeline_metrics.get_metric("h264_decoder.out_frames"), d.rx_nv12_out);
    metric_store(*g_pipeline_metrics.get_metric("h264_decoder.out_bytes"), d.rx_nv12_out_bytes);
    metric_store(*g_pipeline_metrics.get_metric("h264_decoder.dropped_packets"),
                 d.rx_dec_in_err.load(std::memory_order_relaxed) +
                     d.rx_depay_err.load(std::memory_order_relaxed) +
                     d.rx_au_q_drop.load(std::memory_order_relaxed));
    metric_store(*g_pipeline_metrics.get_metric("sdl_sink.dropped_frames"), d.rx_present_q_drop);
}

void telemetry_thread_main(stream_receiver *rcv)
{
    while (g_run.load())
    {
        if (nullptr != rcv)
        {
            sync_peer_link_metrics_live(*rcv);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void publish_latency_metrics(double glass_ms)
{
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
}

}  // namespace vstreamer::apps::rx
