#ifndef VSTREAMER_TEST_APP_PIPELINE_STATE_HPP
#define VSTREAMER_TEST_APP_PIPELINE_STATE_HPP

#include "apps/common/pipeline_state.hpp"
#include "apps/stream_sdl_test/diag.hpp"

#if defined(VSTREAMER_APP_TX_OK)
#include "apps/common/tx/tx_state.hpp"
#endif
#if defined(VSTREAMER_APP_RX_OK)
#include "apps/common/rx/rx_state.hpp"
#endif

#include <atomic>

namespace vstreamer::test_app
{

using apps::g_cpu_map;
using apps::g_pipeline_metrics;
using apps::g_pipeline_metrics_update_mu;
using apps::g_run;
#if defined(VSTREAMER_APP_TX_OK)
using apps::tx::g_tx;
#endif
#if defined(VSTREAMER_APP_RX_OK)
using apps::rx::g_rx;
#endif

extern std::atomic<bool> g_bench_metrics_log;

extern bench_diag g_bench_diag;

}  // namespace vstreamer::test_app

#endif
