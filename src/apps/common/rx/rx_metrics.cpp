#include "apps/common/rx/rx_metrics.hpp"

#include "apps/common/pipeline_state.hpp"
#include "apps/common/stage_latency.hpp"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "core/metrics.hpp"
#include "core/stream_telemetry.hpp"

namespace vstreamer::apps::rx
{

using namespace vstreamer;

/* Receiver-side view of the link; the receiver publishes stream_receiver.* only. The sender's
 * stream_sender.peer_* come from the link reports it receives (tx::sync_sender_peer_link_metrics_live). */
void sync_rx_link_metrics_live(stream_receiver &rcv)
{
    const stream_link_counters cur = rcv.link_counters_snapshot();
    metric_store(*g_pipeline_metrics.get_metric("stream_receiver.udp_packet_received"),
                 cur.udp_packet_received);
    metric_store(*g_pipeline_metrics.get_metric("stream_receiver.udp_gap_count"),
                 cur.udp_gap_count);
    metric_store(*g_pipeline_metrics.get_metric("stream_receiver.fec_packet_received"),
                 cur.fec_packet_received);
    metric_store(*g_pipeline_metrics.get_metric("stream_receiver.fec_gap_count"),
                 cur.fec_gap_count);
    for (const char *key : {"telemetry_sent", "telemetry_send_errors"})
    {
        std::string val;
        if (0 == rcv.query(key, &val))
        {
            metric_store(*g_pipeline_metrics.get_metric(std::string("stream_receiver.") + key),
                         static_cast<uint64_t>(std::strtoull(val.c_str(), nullptr, 10)));
        }
    }
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
    /* dropped_packets is the sum of the first three; the split shows which stage loses AUs. */
    metric_store(*g_pipeline_metrics.get_metric("h264_decoder.depay_errors"), d.rx_depay_err);
    metric_store(*g_pipeline_metrics.get_metric("h264_decoder.au_queue_drops"), d.rx_au_q_drop);
    metric_store(*g_pipeline_metrics.get_metric("h264_decoder.decode_in_errors"), d.rx_dec_in_err);
    /* Output errors include frames the decoder flagged as corrupt and the pipeline discarded. */
    metric_store(*g_pipeline_metrics.get_metric("h264_decoder.decode_out_errors"),
                 d.rx_dec_out_err);
    metric_store(*g_pipeline_metrics.get_metric("sdl_sink.dropped_frames"), d.rx_present_q_drop);
}

void telemetry_thread_main(stream_receiver *rcv)
{
    while (g_run.load())
    {
        if (nullptr != rcv)
        {
            sync_rx_link_metrics_live(*rcv);
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
