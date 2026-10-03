#ifndef VSTREAMER_APPS_RX_RX_STATE_HPP
#define VSTREAMER_APPS_RX_RX_STATE_HPP

#include <atomic>

#include "components/components.hpp"

namespace vstreamer::apps::rx
{

struct rx_state
{
    /* Sender frame rate; converts the source-to-present PTS gap into glass latency. */
    std::atomic<int>  stream_fps {30};
    std::atomic<bool> skip_decode {false};
    std::atomic<bool> dec_opened {false};
};

extern rx_state g_rx;

#if defined(ENABLE_H264_DECODER_MPP)
/* Opens the decoder on first use. -EINVAL when decoding is skipped or dec is null. */
int ensure_decoder_open(vstreamer::h264_decoder_mpp *dec);
#endif

}  // namespace vstreamer::apps::rx

#endif
