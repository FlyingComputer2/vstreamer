#ifndef VSTREAMER_TEST_APP_PIPELINE_STATE_HPP
#define VSTREAMER_TEST_APP_PIPELINE_STATE_HPP

#include "apps/common/cpu_map.hpp"
#include "apps/common/queues.hpp"
#include "apps/common/rx/rx_state.hpp"
#include "apps/common/tx/tx_state.hpp"
#include "test_app/stream_sdl/diag.hpp"

#include <atomic>
#include <mutex>

#include "core/metrics.hpp"

namespace vstreamer::test_app
{

extern std::atomic<bool> g_run;
extern std::atomic<bool> g_diag;
extern std::atomic<bool> g_bench_metrics_log;

extern apps::tx::tx_state g_tx;
extern apps::rx::rx_state g_rx;

extern apps::cpu_stage_map g_cpu_map;

extern bench_diag g_bench_diag;
extern vstreamer::metrics g_pipeline_metrics;

extern std::mutex g_pipeline_metrics_update_mu;

#if defined(ENABLE_H264_DECODER_MPP)
int ensure_decoder_open(vstreamer::h264_decoder_mpp *dec);
#endif

}  // namespace vstreamer::test_app

#endif
