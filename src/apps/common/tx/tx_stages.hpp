#ifndef VSTREAMER_APPS_TX_TX_STAGES_HPP
#define VSTREAMER_APPS_TX_TX_STAGES_HPP

#include "apps/common/queues.hpp"
#include "apps/stream_sdl_test/diag.hpp"

#include "components/components.hpp"

#if defined(ENABLE_H264_ENCODER_MPP) || defined(ENABLE_H264_ENCODER_CEDAR) || \
    defined(ENABLE_H264_ENCODER_INTEL)
#include "apps/stream_sdl_test/encoder_types.hpp"
#endif

namespace vstreamer::apps::tx
{
class source_selector;
}

namespace vstreamer::test_app
{

#if !defined(VSTREAMER_BENCH_RX_ONLY)

bool enqueue_source_frame(vstreamer::data_packet &&raw, apps::pipeline_queue *mjpeg_q,
                          apps::pipeline_queue *nv12_q, bench_diag *diag);

void source_stage_main(vstreamer::component_source *source, apps::pipeline_queue *mjpeg_q,
                       apps::pipeline_queue *nv12_q, bench_diag *diag);
void source_stage_selector_main(apps::tx::source_selector *selector, apps::pipeline_queue *mjpeg_q,
                                apps::pipeline_queue *nv12_q, bench_diag *diag);
void jpeg_stage_main(vstreamer::jpeg_decoder_multicore *jdec, apps::pipeline_queue *mjpeg_q,
                     apps::pipeline_queue *nv12_q, bench_diag *diag, int max_inflight);
#if defined(ENABLE_H264_ENCODER_MPP) || defined(ENABLE_H264_ENCODER_CEDAR) || \
    defined(ENABLE_H264_ENCODER_INTEL)
void encode_stage_main(h264_encoder_t *enc, vstreamer::rtp_h264_pay *pay,
                       vstreamer::stream_sender *sender, apps::pipeline_queue *nv12_q,
                       bench_diag *diag);
#endif

#endif  // !VSTREAMER_BENCH_RX_ONLY

}  // namespace vstreamer::test_app

#endif
