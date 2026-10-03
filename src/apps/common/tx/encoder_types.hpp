#ifndef VSTREAMER_APPS_TX_ENCODER_TYPES_HPP
#define VSTREAMER_APPS_TX_ENCODER_TYPES_HPP

#include "components/components.hpp"

namespace vstreamer::apps::tx
{

#if (!defined(ENABLE_NOISE_SOURCE) && !defined(ENABLE_V4L2_SOURCE)) || !defined(ENABLE_RTP_H264_PAY) || \
    !defined(ENABLE_STREAM_SENDER) ||                                                   \
    (!defined(ENABLE_H264_ENCODER_MPP) && !defined(ENABLE_H264_ENCODER_CEDAR) &&          \
     !defined(ENABLE_H264_ENCODER_INTEL))
#error "TX app set requires noise or v4l2, rtp pay, stream_sender, and one h264 encoder"
#endif

#if defined(ENABLE_H264_ENCODER_MPP)
using encoder_t = vstreamer::h264_encoder_mpp;
#elif defined(ENABLE_H264_ENCODER_CEDAR)
using encoder_t = vstreamer::h264_encoder_cedar;
#elif defined(ENABLE_H264_ENCODER_INTEL)
using encoder_t = vstreamer::h264_encoder_intel;
#else
#error "TX requires ENABLE_H264_ENCODER_MPP, CEDAR, or INTEL"
#endif

}  // namespace vstreamer::apps::tx

#endif
