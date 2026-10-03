#ifndef VSTREAMER_APPS_RX_RX_METRICS_HPP
#define VSTREAMER_APPS_RX_RX_METRICS_HPP

#include "apps/stream_sdl_test/diag.hpp"

namespace vstreamer
{
class stream_receiver;
}

namespace vstreamer::apps::rx
{

void sync_rx_link_metrics_live(vstreamer::stream_receiver &rcv);
void sync_rx_cumulative_counters(const vstreamer::test_app::bench_diag &d,
                                 vstreamer::stream_receiver *rcv);
void telemetry_thread_main(vstreamer::stream_receiver *rcv);
void publish_latency_metrics(double glass_ms);

}  // namespace vstreamer::apps::rx

#endif
