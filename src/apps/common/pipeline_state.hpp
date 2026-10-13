#ifndef VSTREAMER_APPS_PIPELINE_STATE_HPP
#define VSTREAMER_APPS_PIPELINE_STATE_HPP

#include "apps/common/cpu_map.hpp"

#include <atomic>
#include <mutex>

#include "core/metrics.hpp"

namespace vstreamer::apps
{

/* Process-wide state shared by the TX and RX halves. Per-half state is in tx/tx_state.hpp and
 * rx/rx_state.hpp. */

extern std::atomic<bool> g_run;

extern cpu_stage_map g_cpu_map;

extern vstreamer::metrics g_pipeline_metrics;
extern std::mutex         g_pipeline_metrics_update_mu;

/* Stores a node latency under both its latency.* and <component>.* keys; a node that has not
 * measured a frame yet (negative value) is not published. */
void publish_node_latency_metric(const std::atomic<double> &node_ms, const char *latency_key,
                                 const char *component_key);

}  // namespace vstreamer::apps

#endif
