set(VSTREAMER_TEST_SOURCES
    buffer_pool_test.cpp
    component_contract_test.cpp
    camera_noise_mux_test.cpp
    link_emulator_test.cpp
    bench_console_test.cpp
    cpu_map_test.cpp
    h264_level_test.cpp
    key_util_test.cpp
    peer_link_metrics_test.cpp
    pix_convert_test.cpp
    rs_block_erasure_emit_test.cpp
    shared_sized_buffer_test.cpp
)

if(ENABLE_STREAM_SENDER AND ENABLE_STREAM_RECEIVER)
    list(APPEND VSTREAMER_TEST_SOURCES
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

add_executable(vstreamer_tests ${VSTREAMER_TEST_SOURCES}
                              ${CMAKE_SOURCE_DIR}/src/test_app/stream_sdl/cpu_map.cpp
                              ${CMAKE_SOURCE_DIR}/src/test_app/stream_sdl/link_emulator.cpp
                              ${CMAKE_SOURCE_DIR}/src/test_app/stream_sdl/bench_console.cpp)
target_link_libraries(vstreamer_tests PRIVATE GTest::gtest_main vstreamer_source)
target_include_directories(vstreamer_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(vstreamer_tests PRIVATE -Wall -Wextra -Wpedantic)

include(GoogleTest)
gtest_discover_tests(vstreamer_tests DISCOVERY_MODE PRE_TEST)

if(ENABLE_H264_ENCODER_MPP AND ENABLE_H264_DECODER_MPP)
    add_executable(h264_decoder_mpp_hw_test h264_decoder_mpp_test.cpp)
    target_link_libraries(h264_decoder_mpp_hw_test PRIVATE GTest::gtest_main vstreamer_source)
    target_include_directories(h264_decoder_mpp_hw_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
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
    target_include_directories(h264_encoder_mpp_hw_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
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
