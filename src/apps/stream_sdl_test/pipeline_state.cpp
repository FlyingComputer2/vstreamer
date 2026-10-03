/* pipeline_state.cpp — bench-only pipeline state. */

#include "apps/stream_sdl_test/pipeline_state.hpp"

namespace vstreamer::test_app
{

std::atomic<bool> g_bench_metrics_log {false};

bench_diag g_bench_diag;

}  // namespace vstreamer::test_app
