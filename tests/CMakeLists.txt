set(VSTREAMER_TEST_SOURCES
    app_console_test.cpp
    buffer_pool_test.cpp
    component_contract_test.cpp
    link_emulator_test.cpp
    bench_console_test.cpp
    cpu_map_test.cpp
    h264_level_test.cpp
    key_util_test.cpp
    time_util_test.cpp
    component_pdu_test.cpp
    port_caps_test.cpp
    pdu_wakeup_test.cpp
    stage_queue_wakeup_test.cpp
    peer_link_metrics_test.cpp
    pipeline_controller_test.cpp
    stream_header_test.cpp
    stream_telemetry_test.cpp
    pix_convert_test.cpp
    rs_block_erasure_emit_test.cpp
    shared_sized_buffer_test.cpp
)

if(ENABLE_V4L2_SOURCE)
    list(APPEND VSTREAMER_TEST_SOURCES v4l2_source_lifecycle_test.cpp)
endif()

if(ENABLE_V4L2_SOURCE AND ENABLE_NOISE_SOURCE)
    list(APPEND VSTREAMER_TEST_SOURCES source_selector_test.cpp)
endif()

if(ENABLE_JPEG_DECODER_MULTICORE)
    list(APPEND VSTREAMER_TEST_SOURCES
        jpeg_decoder_multicore_test.cpp
        jpeg_stage_inflight_test.cpp)
endif()

if(ENABLE_STREAM_SENDER AND ENABLE_STREAM_RECEIVER)
    list(APPEND VSTREAMER_TEST_SOURCES
        fec_spread_test.cpp
        loopback_test.cpp
        stream_sender_test.cpp
        stream_receiver_test.cpp
    )
endif()

if(ENABLE_RTP_H264_PAY AND ENABLE_RTP_H264_DEPAY)
    list(APPEND VSTREAMER_TEST_SOURCES rtp_h264_test.cpp)
endif()

if(ENABLE_SDL_SINK)
    list(APPEND VSTREAMER_TEST_SOURCES sdl_sink_test.cpp)
endif()

if(ENABLE_MKV_SINK)
    list(APPEND VSTREAMER_TEST_SOURCES mkv_sink_test.cpp)
endif()

if(ENABLE_JPEG_DECODER_MULTICORE OR ENABLE_H264_ENCODER_MPP OR ENABLE_H264_DECODER_MPP)
    list(APPEND VSTREAMER_TEST_SOURCES codec_caps_rejection_test.cpp)
endif()

add_executable(vstreamer_tests ${VSTREAMER_TEST_SOURCES}
                              ${CMAKE_SOURCE_DIR}/src/apps/stream_sdl_test/link_emulator.cpp
                              ${CMAKE_SOURCE_DIR}/src/apps/stream_sdl_test/bench_console.cpp)
target_link_libraries(vstreamer_tests PRIVATE GTest::gtest_main vstreamer_source apps_common)
target_include_directories(vstreamer_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests)
target_compile_options(vstreamer_tests PRIVATE -Wall -Wextra -Wpedantic)

include(GoogleTest)
gtest_discover_tests(vstreamer_tests DISCOVERY_MODE PRE_TEST)

if(TARGET uvc_stream_sender AND TARGET sdl_stream_receiver)
    add_executable(split_apps_test split_apps_test.cpp)
    target_compile_definitions(split_apps_test PRIVATE
        UVC_STREAM_SENDER_BIN="$<TARGET_FILE:uvc_stream_sender>"
        SDL_STREAM_RECEIVER_BIN="$<TARGET_FILE:sdl_stream_receiver>")
    target_link_libraries(split_apps_test PRIVATE GTest::gtest_main)
    target_compile_options(split_apps_test PRIVATE -Wall -Wextra -Wpedantic)
    add_dependencies(split_apps_test uvc_stream_sender sdl_stream_receiver)
    gtest_discover_tests(
        split_apps_test
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            LABELS "hw"
            TIMEOUT 60
            RESOURCE_LOCK "mpp_hw"
    )
endif()

if(ENABLE_H264_ENCODER_MPP AND ENABLE_H264_DECODER_MPP)
    add_executable(h264_decoder_mpp_hw_test h264_decoder_mpp_test.cpp)
    target_link_libraries(h264_decoder_mpp_hw_test PRIVATE GTest::gtest_main vstreamer_source)
    target_include_directories(h264_decoder_mpp_hw_test PRIVATE ${CMAKE_SOURCE_DIR}/src
                                                                 ${CMAKE_SOURCE_DIR}/tests)
    target_compile_options(h264_decoder_mpp_hw_test PRIVATE -Wall -Wextra -Wpedantic)
    get_filename_component(_tsan_mpp_supp_dec "${CMAKE_CURRENT_SOURCE_DIR}/tsan_mpp.supp" ABSOLUTE)
    gtest_discover_tests(
        h264_decoder_mpp_hw_test
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            LABELS "hw"
            TIMEOUT 20
            RESOURCE_LOCK "mpp_hw"
            ENVIRONMENT "TSAN_OPTIONS=suppressions=${_tsan_mpp_supp_dec}:halt_on_error=1"
    )
endif()

if(ENABLE_H264_ENCODER_MPP)
    add_executable(h264_encoder_mpp_hw_test h264_encoder_mpp_test.cpp)
    target_link_libraries(h264_encoder_mpp_hw_test PRIVATE GTest::gtest_main vstreamer_source)
    target_include_directories(h264_encoder_mpp_hw_test PRIVATE ${CMAKE_SOURCE_DIR}/src
                                                                 ${CMAKE_SOURCE_DIR}/tests)
    target_compile_options(h264_encoder_mpp_hw_test PRIVATE -Wall -Wextra -Wpedantic)
    get_filename_component(_tsan_mpp_supp "${CMAKE_CURRENT_SOURCE_DIR}/tsan_mpp.supp" ABSOLUTE)
    gtest_discover_tests(
        h264_encoder_mpp_hw_test
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            LABELS "hw"
            TIMEOUT 20
            RESOURCE_LOCK "mpp_hw"
            ENVIRONMENT "TSAN_OPTIONS=suppressions=${_tsan_mpp_supp}:halt_on_error=1"
    )
endif()

if(ENABLE_H264_ENCODER_INTEL)
    add_executable(h264_encoder_intel_test h264_encoder_intel_test.cpp)
    target_link_libraries(h264_encoder_intel_test PRIVATE GTest::gtest_main vstreamer_source)
    target_include_directories(h264_encoder_intel_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(h264_encoder_intel_test PRIVATE -Wall -Wextra -Wpedantic)
    gtest_discover_tests(h264_encoder_intel_test DISCOVERY_MODE PRE_TEST)

    add_executable(h264_encoder_intel_hw_test h264_encoder_intel_hw_test.cpp)
    target_link_libraries(h264_encoder_intel_hw_test PRIVATE GTest::gtest_main vstreamer_source)
    target_include_directories(h264_encoder_intel_hw_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(h264_encoder_intel_hw_test PRIVATE -Wall -Wextra -Wpedantic)
    gtest_discover_tests(
        h264_encoder_intel_hw_test
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            LABELS "hw"
            TIMEOUT 120
    )
endif()
