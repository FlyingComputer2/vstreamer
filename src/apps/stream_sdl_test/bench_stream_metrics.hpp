#ifndef VSTREAMER_STREAM_SDL_TEST_BENCH_STREAM_METRICS_HPP
#define VSTREAMER_STREAM_SDL_TEST_BENCH_STREAM_METRICS_HPP

#include "apps/stream_sdl_test/channel_controller.hpp"

#include <cstdint>

namespace vstreamer::stream_sdl_test
{

void publish_stream_sdl_status_metrics(const test_app::channel_controller *channel, bool kmsdrm,
                                       double present_fps, bool pipeline_flowing, double glass_ms);
void publish_channel_rate_metrics(const test_app::channel_controller *channel, double dt,
                                  bool have_snap, uint64_t prev_ch_bytes_in,
                                  uint64_t prev_ch_bytes_out, uint64_t prev_ch_fwd_drops,
                                  const test_app::channel_controller::forward_stats &ch);

}  // namespace vstreamer::stream_sdl_test

#endif
