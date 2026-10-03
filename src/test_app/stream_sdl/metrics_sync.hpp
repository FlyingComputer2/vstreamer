#ifndef VSTREAMER_TEST_APP_METRICS_SYNC_HPP
#define VSTREAMER_TEST_APP_METRICS_SYNC_HPP

#include "test_app/stream_sdl/channel_controller.hpp"
#include "test_app/stream_sdl/diag.hpp"
#include "apps/common/queues.hpp"

#include "components/components.hpp"

namespace vstreamer::test_app
{

bool query_source_metric_string(vstreamer::component_source *src, const char *key,
                                std::string &out);
void store_source_pipeline_metrics(double source_out_fps, double source_out_kbps, const char *ts,
                                   vstreamer::component *jdec);

void update_pipeline_metrics(const bench_diag &d, vstreamer::component_coder *enc,
                             vstreamer::stream_sender *sender, vstreamer::stream_receiver *rcv,
                             vstreamer::component_sink *preview, bool kmsdrm,
                             pipeline_rate_state &rate, const channel_controller *channel,
                             vstreamer::component *jdec, bool jpeg_active,
                             vstreamer::component *dec);

void sync_pipeline_metrics_live(const bench_diag &d, vstreamer::stream_sender *sender,
                                vstreamer::component_coder *enc, vstreamer::stream_receiver *rcv,
                                const channel_controller *channel);

void telemetry_thread_main(vstreamer::stream_receiver *rcv);

int query_encoder_qp(vstreamer::component_coder &enc);
int query_encoder_cbr_bps(vstreamer::component_coder &enc);

}  // namespace vstreamer::test_app

#endif
