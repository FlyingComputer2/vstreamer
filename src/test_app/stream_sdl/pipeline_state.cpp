/* pipeline_state.cpp — split from stream_sdl (P11-T4). */

#include "test_app/stream_sdl/pipeline_state.hpp"

#include "components/components.hpp"

namespace vstreamer::test_app
{

using namespace vstreamer;

std::atomic<bool> g_run {true};
std::atomic<bool> g_diag {false};
std::atomic<bool> g_bench_metrics_log {false};

apps::tx::tx_state g_tx;
apps::rx::rx_state g_rx;

#if defined(ENABLE_H264_DECODER_MPP)
int ensure_decoder_open(h264_decoder_mpp *dec)
{
    if (g_rx.skip_decode.load() || nullptr == dec)
    {
        return -EINVAL;
    }
    if (g_rx.dec_opened.load())
    {
        return 0;
    }
    const int r = dec->open();
    if (r < 0)
    {
        return r;
    }
    g_rx.dec_opened = true;
    return 0;
}
#endif

apps::cpu_stage_map g_cpu_map;

bench_diag        g_bench_diag;
metrics           g_pipeline_metrics;
std::mutex        g_pipeline_metrics_update_mu;

}  // namespace vstreamer::test_app
