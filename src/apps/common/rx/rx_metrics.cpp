#include "apps/common/rx/rx_metrics.hpp"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "apps/common/pipeline_state.hpp"
#include "apps/common/stage_latency.hpp"
#include "core/metrics.hpp"
#include "core/stream_metrics.hpp"

namespace vstreamer::apps::rx
{

using namespace vstreamer;

/* Receiver-side view of the link; the receiver publishes stream_receiver.* only. The sender's
 * stream_sender.peer_* come from the link reports it receives (tx::sync_sender_peer_link_metrics_live). */
void sync_rx_link_metrics_live(stream_receiver &rcv)
{
    stream_metric_tuple tuple;
    for (const auto &layer : {"udp", "fec"})
    {
        if (query_stream_metric(rcv, std::string(layer) + "_pkt_recv", &tuple))
        {
            const std::string prefix = std::string("stream_receiver.") + layer;
            metric_store(
                *g_pipeline_metrics.get_metric(prefix + "_packet_received"),
                tuple.first);
            metric_store(*g_pipeline_metrics.get_metric(prefix + "_gap_count"),
                         tuple.second);
        }
    }
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
    stream_metric_tuple tuple;
    if (query_stream_metric(*rcv, "fec_pkt_recv", &tuple))
    {
        metric_store(
            *g_pipeline_metrics.get_metric("stream_receiver.in_packets"),
            tuple.first);
    }
    if (query_stream_metric(*rcv, "byt_recv", &tuple))
    {
        metric_store(
            *g_pipeline_metrics.get_metric("stream_receiver.out_bytes"),
            tuple.second);
    }

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
    metric_store(*g_pipeline_metrics.get_metric("latency.depay_ms"),
                 apps::g_latency_depay_ms.load(std::memory_order_relaxed));
    metric_store(*g_pipeline_metrics.get_metric("latency.dec_in_ms"),
                 apps::g_latency_dec_in_ms.load(std::memory_order_relaxed));
    metric_store(*g_pipeline_metrics.get_metric("latency.dec_out_ms"),
                 apps::g_latency_dec_out_ms.load(std::memory_order_relaxed));
    metric_store(*g_pipeline_metrics.get_metric("latency.present_ms"),
                 apps::g_latency_present_ms.load(std::memory_order_relaxed));

    publish_node_latency_metric(apps::g_node_latency_dec_ms, "latency.dec_node_ms",
                                "h264_decoder.node_latency_ms");
    publish_node_latency_metric(apps::g_node_latency_present_ms, "latency.present_node_ms",
                                "sdl_sink.node_latency_ms");
}

#if defined(VSTREAMER_APP_SPLIT_RX_ONLY)

bool latency_metric_visible(std::string_view full_name)
{
    return full_name != "latency.source_ms" && full_name != "latency.jpeg_ms" &&
           full_name != "latency.enc_in_ms" && full_name != "latency.enc_out_ms";
}

#endif

}  // namespace vstreamer::apps::rx
