#include "apps/stream_sdl_test/bench_stream_metrics.hpp"

#include "apps/common/app_metrics.hpp"
#include "apps/stream_sdl_test/channel_controller.hpp"
#include "apps/stream_sdl_test/pipeline_state.hpp"

#include "core/metrics.hpp"

namespace vstreamer::stream_sdl_test
{

using test_app::g_pipeline_metrics;

void publish_channel_rate_metrics(const test_app::channel_controller *channel, double dt,
                                  bool have_snap, uint64_t prev_ch_bytes_in,
                                  uint64_t prev_ch_bytes_out, uint64_t prev_ch_fwd_drops,
                                  const test_app::channel_controller::forward_stats &ch)
{
    if (nullptr == channel)
    {
        return;
    }
    double ch_fwd_kbps = 0.0;
    uint64_t ch_fwd_drops = 0;
    double ch_drop_pps = 0.0;
    double ch_drop_kbps = 0.0;
    ch_fwd_kbps = ch.bytes_out > prev_ch_bytes_out
                      ? static_cast<double>(ch.bytes_out - prev_ch_bytes_out) * 8.0 / dt / 1000.0
                      : 0.0;
    ch_fwd_drops = ch.dropped_rate + ch.dropped_loss + ch.dropped_queue;
    ch_drop_pps = apps::rate_per_sec(ch_fwd_drops, prev_ch_fwd_drops, dt);
    if (have_snap && dt > 0.0 && ch.bytes_in >= prev_ch_bytes_in && ch.bytes_out >= prev_ch_bytes_out)
    {
        const uint64_t din = ch.bytes_in - prev_ch_bytes_in;
        const uint64_t dout = ch.bytes_out - prev_ch_bytes_out;
        if (din > dout)
        {
            ch_drop_kbps = static_cast<double>(din - dout) * 8.0 / dt / 1000.0;
        }
    }
    metric_store(*g_pipeline_metrics.get_metric("channel.forward_kbps"), ch_fwd_kbps);
    metric_store(*g_pipeline_metrics.get_metric("channel.dropped_pps"), ch_drop_pps);
    metric_store(*g_pipeline_metrics.get_metric("channel.dropped_kbps"), ch_drop_kbps);
    metric_store(*g_pipeline_metrics.get_metric("channel.max_kbps"), channel->max_kbps());
    metric_store(*g_pipeline_metrics.get_metric("channel.constant_loss_pct"),
                 channel->constant_loss());
    metric_store(*g_pipeline_metrics.get_metric("channel.queue"),
                 static_cast<double>(channel->forward_queue_size()));
}

}  // namespace vstreamer::stream_sdl_test
