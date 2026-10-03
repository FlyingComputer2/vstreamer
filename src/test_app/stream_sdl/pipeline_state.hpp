#ifndef VSTREAMER_TEST_APP_PIPELINE_STATE_HPP
#define VSTREAMER_TEST_APP_PIPELINE_STATE_HPP

#include "apps/common/cpu_map.hpp"
#include "apps/common/queues.hpp"
#include "test_app/stream_sdl/diag.hpp"

#include <atomic>
#include <mutex>

#include "core/component_source.hpp"
#include "core/metrics.hpp"

namespace vstreamer::test_app
{

extern std::atomic<bool> g_run;
extern std::atomic<bool> g_diag;
extern std::atomic<bool> g_bench_metrics_log;
extern std::atomic<bool> g_skip_decode;
extern std::atomic<bool> g_dec_opened;

extern apps::cpu_stage_map g_cpu_map;

extern std::atomic<int> g_stream_fps;
extern std::atomic<int> g_pending_console_cbr_kbps;
extern std::atomic<int> g_pending_console_qp;
extern std::atomic<int> g_pending_console_gop;
extern std::atomic<bool> g_pending_console_idr;

extern bench_diag g_bench_diag;
extern vstreamer::metrics g_pipeline_metrics;
extern vstreamer::component_source *g_metrics_source;
extern apps::pipeline_queue *g_metrics_nv12_q;

extern std::mutex g_pipeline_metrics_update_mu;

#if defined(ENABLE_H264_DECODER_MPP)
int ensure_decoder_open(vstreamer::h264_decoder_mpp *dec);
#endif

}  // namespace vstreamer::test_app

#endif
