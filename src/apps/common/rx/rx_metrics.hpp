#ifndef VSTREAMER_APPS_RX_RX_METRICS_HPP
#define VSTREAMER_APPS_RX_RX_METRICS_HPP

#include "apps/stream_sdl_test/diag.hpp"

#include <string_view>

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

#if defined(VSTREAMER_APP_SPLIT_RX_ONLY)
/* Hide TX-only latency.* keys from GS console dumps (split receiver has no TX stages). */
bool latency_metric_visible(std::string_view full_name);
#endif

}  // namespace vstreamer::apps::rx

#endif
