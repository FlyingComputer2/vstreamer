#ifndef VSTREAMER_TEST_APP_STAGES_HPP
#define VSTREAMER_TEST_APP_STAGES_HPP

#include "apps/common/queues.hpp"
#include "test_app/stream_sdl/diag.hpp"
#include "test_app/stream_sdl/encoder_types.hpp"

#include "components/components.hpp"

namespace vstreamer::apps::tx
{
class source_selector;
}

namespace vstreamer::test_app
{

int cfg_str(vstreamer::component &c, const char *key, const char *val);
int open_stage(const char *name, int rc);
int prepare_preview_sink(vstreamer::component_sink *preview, bool kmsdrm, int w, int h);

void source_stage_main(vstreamer::component_source *source, apps::pipeline_queue *mjpeg_q,
                       apps::pipeline_queue *nv12_q, bench_diag *diag);
bool enqueue_source_frame(vstreamer::data_packet &&raw, apps::pipeline_queue *mjpeg_q,
                          apps::pipeline_queue *nv12_q, bench_diag *diag);

void source_stage_selector_main(apps::tx::source_selector *selector, apps::pipeline_queue *mjpeg_q,
                                apps::pipeline_queue *nv12_q, bench_diag *diag);
void jpeg_stage_main(vstreamer::jpeg_decoder_multicore *jdec, apps::pipeline_queue *mjpeg_q,
                     apps::pipeline_queue *nv12_q, bench_diag *diag, int max_inflight);
void encode_stage_main(h264_encoder_t *enc, vstreamer::rtp_h264_pay *pay,
                       vstreamer::stream_sender *sender, apps::pipeline_queue *nv12_q,
                       bench_diag *diag);
void present_thread_main(vstreamer::component_sink *display, apps::present_frame_queue *present_q,
                         int width, int height, bool kmsdrm, bool sdl_open_on_thread,
                         bench_diag *diag);
void rx_net_thread_main(vstreamer::stream_receiver *rcv, vstreamer::rtp_h264_depay *depay,
                        apps::rx_au_queue *au_q, bench_diag *diag);
void decode_thread_main(vstreamer::h264_decoder_mpp *dec, apps::present_frame_queue *present_q,
                        apps::rx_au_queue *au_q, bench_diag *diag);

}  // namespace vstreamer::test_app

#endif
