if(NOT ENABLE_JPEG_DECODER_MULTICORE OR NOT ENABLE_SDL_SINK OR NOT ENABLE_STREAM_SENDER OR
   NOT ENABLE_STREAM_RECEIVER OR NOT ENABLE_RTP_H264_PAY OR NOT ENABLE_RTP_H264_DEPAY OR
   NOT ENABLE_H264_DECODER_MPP)
    message(FATAL_ERROR "ENABLE_TEST_STREAM_SDL requires jpeg_decoder, stream_*, rtp, mpp decode, sdl")
endif()
if(NOT ENABLE_NOISE_SOURCE AND NOT ENABLE_V4L2_SOURCE)
    message(FATAL_ERROR "ENABLE_TEST_STREAM_SDL requires ENABLE_NOISE_SOURCE or ENABLE_V4L2_SOURCE")
endif()
if(NOT ENABLE_H264_ENCODER_MPP AND NOT ENABLE_H264_ENCODER_CEDAR AND NOT ENABLE_H264_ENCODER_INTEL)
    message(FATAL_ERROR "ENABLE_TEST_STREAM_SDL requires ENABLE_H264_ENCODER_MPP, CEDAR, or INTEL")
endif()

set(_stream_sdl_sources
    main.cpp
    pipeline_state.cpp
    diag.cpp
    metrics_sync.cpp
    stages.cpp
    self_test.cpp
    channel_controller.cpp
    link_emulator.cpp
    bench_console.cpp
    cpu_map.cpp
)
if(ENABLE_V4L2_SOURCE AND ENABLE_NOISE_SOURCE)
    list(APPEND _stream_sdl_sources camera_noise_mux.cpp)
endif()

vstreamer_add_app(stream_sdl ${_stream_sdl_sources} LSAN)
