#ifndef VSTREAMER_APPS_RX_RX_STAGES_HPP
#define VSTREAMER_APPS_RX_RX_STAGES_HPP

#include "apps/common/queues.hpp"
#include "apps/stream_sdl_test/diag.hpp"

#include "components/components.hpp"

namespace vstreamer::test_app
{

int prepare_preview_sink(vstreamer::component_sink *preview, bool kmsdrm, int w, int h);

#if !defined(VSTREAMER_BENCH_TX_ONLY)

void present_thread_main(vstreamer::component_sink *display, apps::present_pdu_queue *present_queue,
                         int width, int height, bool kmsdrm, bool sdl_open_on_thread,
                         bench_diag *diag);
void rx_net_thread_main(vstreamer::stream_receiver *rcv, vstreamer::rtp_h264_depay *depay,
                        apps::pdu_rx_au_queue *au_in_queue, bench_diag *diag);
#if defined(ENABLE_H264_DECODER_MPP)
void decode_thread_main(vstreamer::h264_decoder_mpp *dec, apps::present_pdu_queue *present_queue,
                        apps::pdu_rx_au_queue *au_in_queue, bench_diag *diag);
#endif

#endif

}  // namespace vstreamer::test_app

#endif
