# Core sources and the components that build on every platform (Linux and ESP-IDF),
# relative to the repository root. Reads the ENABLE_* switches; appends to
# VSTREAMER_SOURCES. ISA-L is the caller's job when VSTREAMER_NEEDS_ISAL is set.

list(APPEND VSTREAMER_SOURCES
    src/core/metrics.cpp
    src/core/sdu_type.cpp
    src/core/sdu_caps.cpp
    src/core/port_caps.cpp
    src/core/pdu_wakeup.cpp
    src/core/buffer_pool.cpp
    src/core/component_sink.cpp
    src/core/rtp_h264.cpp
    src/core/pix_convert.cpp
    src/core/h264_level.cpp
    src/core/h264_sps.cpp
    src/core/mkv_writer.cpp
    src/core/component_factory.cpp
    src/core/pipeline.cpp
    src/core/pipeline_queue.cpp
)

set(VSTREAMER_NEEDS_ISAL OFF)
if(ENABLE_MKV_SINK)
    list(APPEND VSTREAMER_SOURCES src/components/mkv_sink.cpp)
endif()
if(ENABLE_STREAM_SENDER OR ENABLE_STREAM_RECEIVER)
    list(APPEND VSTREAMER_SOURCES src/core/rs_block_erasure.cpp)
    set(VSTREAMER_NEEDS_ISAL ON)
endif()
if(ENABLE_STREAM_SENDER)
    list(APPEND VSTREAMER_SOURCES src/components/stream_sender.cpp)
endif()
if(ENABLE_STREAM_RECEIVER)
    list(APPEND VSTREAMER_SOURCES src/components/stream_receiver.cpp)
endif()
if(ENABLE_RTP_H264_PAY)
    list(APPEND VSTREAMER_SOURCES src/components/rtp_h264_pay.cpp)
endif()
if(ENABLE_RTP_H264_DEPAY)
    list(APPEND VSTREAMER_SOURCES src/components/rtp_h264_depay.cpp)
endif()
