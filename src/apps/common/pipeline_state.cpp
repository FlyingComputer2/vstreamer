/* pipeline_state.cpp — process-wide state shared by the TX and RX halves. */

#include "apps/common/pipeline_state.hpp"

namespace vstreamer::apps
{

std::atomic<bool> g_run {true};

cpu_stage_map g_cpu_map;

vstreamer::metrics g_pipeline_metrics;
std::mutex         g_pipeline_metrics_update_mu;

void publish_node_latency_metric(const std::atomic<double> &node_ms, const char *latency_key,
                                 const char *component_key)
{
    const double ms = node_ms.load(std::memory_order_relaxed);
    if (ms < 0.0)
    {
        return;
    }
    metric_store(*g_pipeline_metrics.get_metric(latency_key), ms);
    metric_store(*g_pipeline_metrics.get_metric(component_key), ms);
}

}  // namespace vstreamer::apps
