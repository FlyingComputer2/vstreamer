#ifndef VSTREAMER_TEST_APP_METRICS_SYNC_HPP
#define VSTREAMER_TEST_APP_METRICS_SYNC_HPP

#include "apps/common/rx/rx_metrics.hpp"
#if !defined(VSTREAMER_BENCH_RX_ONLY)
#include "apps/common/tx/tx_metrics.hpp"
#endif
#include "apps/stream_sdl_test/channel_controller.hpp"
#include "apps/stream_sdl_test/diag.hpp"
#include "apps/common/queues.hpp"

#include "components/components.hpp"

namespace vstreamer::test_app
{

#if !defined(VSTREAMER_BENCH_RX_ONLY)
using apps::tx::query_encoder_cbr_bps;
using apps::tx::query_encoder_qp;
using apps::tx::query_source_metric_string;
using apps::tx::store_source_pipeline_metrics;
#endif

void update_pipeline_metrics(const bench_diag &d, vstreamer::component_coder *enc,
                             vstreamer::stream_sender *sender, vstreamer::stream_receiver *rcv,
                             vstreamer::component_sink *preview, bool kmsdrm,
                             pipeline_rate_state &rate, const channel_controller *channel,
                             vstreamer::component *jdec, bool jpeg_active,
                             vstreamer::component *dec,
                             vstreamer::component_coder *depay = nullptr);

void sync_pipeline_metrics_live(const bench_diag &d, vstreamer::stream_sender *sender,
                                vstreamer::component_coder *enc, vstreamer::stream_receiver *rcv,
                                const channel_controller *channel);

}  // namespace vstreamer::test_app

#endif
