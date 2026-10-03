/* pipeline_state.cpp — split from stream_sdl (P11-T4). */

#include "test_app/stream_sdl/pipeline_state.hpp"

#include "test_app/stream_sdl/encoder_types.hpp"
#include "test_app/stream_sdl/queues.hpp"

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
cpu_stage_map g_cpu_map;

std::atomic<int64_t> g_latest_source_pts {0};
std::atomic<int>     g_stream_fps {30};
std::atomic<int>     g_pending_console_cbr_kbps {-1};
std::atomic<int>     g_pending_console_qp {-1};
std::atomic<int>     g_pending_console_gop {-1};
std::atomic<bool>    g_pending_console_idr {false};
std::atomic<double>  g_glass_latency_ms {0.0};
std::atomic<double>  g_latency_source_ms {0.0};
std::atomic<double>  g_latency_jpeg_ms {0.0};
std::atomic<double>  g_latency_enc_in_ms {0.0};
std::atomic<double>  g_latency_enc_out_ms {0.0};
std::atomic<double>  g_latency_depay_ms {0.0};
std::atomic<double>  g_latency_dec_in_ms {0.0};
std::atomic<double>  g_latency_dec_out_ms {0.0};
std::atomic<double>  g_latency_present_ms {0.0};

bench_diag           g_bench_diag;
metrics              g_pipeline_metrics;
component_source    *g_metrics_source = nullptr;
pipeline_queue      *g_metrics_nv12_q = nullptr;
std::mutex           g_pipeline_metrics_update_mu;

}  // namespace vstreamer::test_app
