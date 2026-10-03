#ifndef VSTREAMER_TEST_APP_ENCODER_TYPES_HPP
#define VSTREAMER_TEST_APP_ENCODER_TYPES_HPP

#include "components/components.hpp"

namespace vstreamer::test_app
{

#if (!defined(ENABLE_NOISE_SOURCE) && !defined(ENABLE_V4L2_SOURCE)) ||              \
    !defined(ENABLE_JPEG_DECODER_MULTICORE) || !defined(ENABLE_RTP_H264_PAY) ||    \
    !defined(ENABLE_STREAM_SENDER) || !defined(ENABLE_STREAM_RECEIVER) ||          \
    !defined(ENABLE_RTP_H264_DEPAY) || !defined(ENABLE_H264_DECODER_MPP) ||         \
    !defined(ENABLE_SDL_SINK)
#error "stream_sdl requires noise or v4l2, jpeg_decoder, stream_*, rtp h264, mpp decode, sdl"
#endif

#if defined(ENABLE_H264_ENCODER_MPP)
using h264_encoder_t = vstreamer::h264_encoder_mpp;
#elif defined(ENABLE_H264_ENCODER_CEDAR)
using h264_encoder_t = vstreamer::h264_encoder_cedar;
#elif defined(ENABLE_H264_ENCODER_INTEL)
using h264_encoder_t = vstreamer::h264_encoder_intel;
#else
#error "stream_sdl requires ENABLE_H264_ENCODER_MPP, CEDAR, or INTEL"
#endif

}  // namespace vstreamer::test_app

#endif
