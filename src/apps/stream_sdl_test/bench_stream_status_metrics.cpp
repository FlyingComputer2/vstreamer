#include "apps/stream_sdl_test/bench_stream_metrics.hpp"

#include "apps/common/app_metrics.hpp"
#include "apps/stream_sdl_test/pipeline_state.hpp"

#include "core/metrics.hpp"

namespace vstreamer::stream_sdl_test
{

using test_app::g_pipeline_metrics;

void publish_stream_sdl_status_metrics(const test_app::channel_controller *channel, bool kmsdrm,
                                       double present_fps, bool pipeline_flowing, double glass_ms)
{
    (void)channel;
    metric_store(*g_pipeline_metrics.get_metric("stream_sdl.status"), "running");
    metric_store(*g_pipeline_metrics.get_metric("stream_sdl.display"), kmsdrm ? "kmsdrm" : "sdl");
    if (kmsdrm)
    {
        const char *note = present_fps > 0.5 ? "kmsdrm presenting decoded frames"
                                             : "kmsdrm active; waiting for decode/present";
        metric_store(*g_pipeline_metrics.get_metric("stream_sdl.note"), note);
    }
    metric_store(*g_pipeline_metrics.get_metric("stream_sdl.pipeline_ok"),
                 pipeline_flowing ? "yes" : "warming");
    metric_store(*g_pipeline_metrics.get_metric("stream_sdl.glass_latency_ms"), glass_ms);
}

}  // namespace vstreamer::stream_sdl_test
