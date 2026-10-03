/* pipeline_state.cpp — split from stream_sdl (P11-T4). */

#include "test_app/stream_sdl/pipeline_state.hpp"

#include "test_app/stream_sdl/encoder_types.hpp"

#include "components/components.hpp"

namespace vstreamer::test_app
{

using namespace vstreamer;

std::atomic<bool> g_run {true};
std::atomic<bool> g_diag {false};
std::atomic<bool> g_bench_metrics_log {false};
std::atomic<bool> g_skip_decode {false};
std::atomic<bool> g_dec_opened {false};

int ensure_decoder_open(h264_decoder_mpp *dec)
{
    if (g_skip_decode.load() || nullptr == dec)
    {
        return -EINVAL;
    }
    if (g_dec_opened.load())
    {
        return 0;
    }
    const int r = dec->open();
    if (r < 0)
    {
        return r;
    }
    g_dec_opened = true;
    return 0;
}
apps::cpu_stage_map g_cpu_map;

std::atomic<int>  g_stream_fps {30};
std::atomic<int>  g_pending_console_cbr_kbps {-1};
std::atomic<int>  g_pending_console_qp {-1};
std::atomic<int>  g_pending_console_gop {-1};
std::atomic<bool> g_pending_console_idr {false};

bench_diag        g_bench_diag;
metrics           g_pipeline_metrics;
component_source *g_metrics_source = nullptr;
apps::pipeline_queue *g_metrics_nv12_q = nullptr;
std::mutex           g_pipeline_metrics_update_mu;

}  // namespace vstreamer::test_app
