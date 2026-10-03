#ifndef VSTREAMER_APPS_RX_RX_REQUIRE_HPP
#define VSTREAMER_APPS_RX_RX_REQUIRE_HPP

#if !defined(ENABLE_STREAM_RECEIVER) || !defined(ENABLE_RTP_H264_DEPAY) || \
    !defined(ENABLE_H264_DECODER_MPP) || !defined(ENABLE_SDL_SINK)
#error "RX app set requires stream_receiver, rtp depay, h264 decoder MPP, and SDL sink"
#endif

#endif
