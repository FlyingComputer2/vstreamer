/* pipeline_state.cpp — process-wide state shared by the TX and RX halves. */

#include "apps/common/pipeline_state.hpp"

namespace vstreamer::apps
{

std::atomic<bool> g_run {true};

cpu_stage_map g_cpu_map;

vstreamer::metrics g_pipeline_metrics;
std::mutex         g_pipeline_metrics_update_mu;

}  // namespace vstreamer::apps
