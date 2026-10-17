#ifndef VSTREAMER_TEST_APP_CHANNEL_PORTS_HPP
#define VSTREAMER_TEST_APP_CHANNEL_PORTS_HPP

#include "apps/common/link_emulator.hpp"

namespace vstreamer::test_app
{

/* Loopback bench defaults (stream_sdl and channel_controller). */
inline constexpr const char k_loopback_host[] = "127.0.0.1";

inline constexpr int k_chan_fwd_ingress = 5000;   /* stream_sender → channel */
inline constexpr int k_stream_rx_listen = 5001;   /* channel fwd → stream_receiver */
inline constexpr int k_chan_console = 5090; /* UDP console: impairment + pipeline metrics */

/* Default forward-path cap (stream_sender → stream_receiver). 0 = unlimited. */
inline constexpr double k_chan_default_max_kbps = 0.0;

inline constexpr int k_chan_default_drop_dt_ms = apps::k_link_default_drop_dt_ms;
inline constexpr int k_chan_min_drop_dt_ms = apps::k_link_min_drop_dt_ms;
inline constexpr int k_chan_max_drop_dt_ms = apps::k_link_max_drop_dt_ms;
inline constexpr int k_chan_default_queue_depth = apps::k_link_default_queue_depth;

using apps::link_emulator;

/* Default MPP CBR target for stream_sdl (metrics use kbps). */
inline constexpr int k_encoder_default_cbr_kbps = 20'000;
inline constexpr int k_encoder_default_cbr_bps = k_encoder_default_cbr_kbps * 1000;

}  // namespace vstreamer::test_app

#endif  // VSTREAMER_TEST_APP_CHANNEL_PORTS_HPP
