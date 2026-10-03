cmake_minimum_required(VERSION 3.16)
project(vstreamer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_NOISE_SOURCE "Build noise_source component" ON)
option(ENABLE_V4L2_SOURCE "Build v4l2_source component" ON)
option(ENABLE_JPEG_DECODER_MULTICORE "Build jpeg_decoder_multicore component" ON)
option(ENABLE_H264_DECODER_MPP "Build h264_decoder_mpp component" ON)
option(ENABLE_H264_ENCODER_MPP "Build h264_encoder_mpp component (Rockchip)" OFF)
option(ENABLE_H264_ENCODER_CEDAR "Build h264_encoder_cedar component" OFF)
option(ENABLE_H264_ENCODER_INTEL "Build h264_encoder_intel component" OFF)
option(ENABLE_MKV_SINK "Build mkv_sink component" ON)
option(ENABLE_SDL_SINK "Build sdl_sink display component" OFF)
option(ENABLE_STREAM_SENDER "Build stream_sender component" ON)
option(ENABLE_STREAM_RECEIVER "Build stream_receiver component" ON)
option(ENABLE_RTP_H264_PAY "Build rtp_h264_pay component" ON)
option(ENABLE_RTP_H264_DEPAY "Build rtp_h264_depay component" ON)
option(VSTREAMER_BUILD_TESTS "Build unit/integration tests" OFF)


find_package(PkgConfig REQUIRED)

if(ENABLE_H264_DECODER_MPP OR ENABLE_H264_ENCODER_MPP)
    pkg_check_modules(MPP IMPORTED_TARGET rockchip_mpp)
    if(ENABLE_H264_DECODER_MPP AND NOT MPP_FOUND)
        message(WARNING "rockchip_mpp not found; disabling ENABLE_H264_DECODER_MPP")
        set(ENABLE_H264_DECODER_MPP OFF CACHE BOOL "" FORCE)
    endif()
    if(ENABLE_H264_ENCODER_MPP AND NOT MPP_FOUND)
        message(WARNING "rockchip_mpp not found; disabling ENABLE_H264_ENCODER_MPP")
        set(ENABLE_H264_ENCODER_MPP OFF CACHE BOOL "" FORCE)
    endif()
endif()

set(VSTREAMER_SOURCES
    src/core/frame.cpp
    src/core/metrics.cpp
    src/core/data_packet.cpp
    src/core/buffer_pool.cpp
    src/core/component_sink.cpp
    src/core/rtp_h264.cpp
    src/core/pix_convert.cpp
    src/core/h264_level.cpp
    src/core/component_factory.cpp
)

if(ENABLE_NOISE_SOURCE)
    set(PFFFT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(PFFFT_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(PFFFT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(PFFFT_USE_TYPE_DOUBLE OFF CACHE BOOL "" FORCE)
    set(PFFFT_USE_SIMD ON CACHE BOOL "" FORCE)
    set(INSTALL_PFFFT OFF CACHE BOOL "" FORCE)

    include(FetchContent)
    FetchContent_Declare(pffft
        GIT_REPOSITORY https://github.com/marton78/pffft.git
        GIT_TAG        e1dbebc9fbf74247d12f094accbbc470aaee8715
    )
    FetchContent_GetProperties(pffft)
    if(NOT pffft_POPULATED)
        FetchContent_Populate(pffft)
    endif()
    add_subdirectory(${pffft_SOURCE_DIR} ${pffft_BINARY_DIR} EXCLUDE_FROM_ALL)

    list(APPEND VSTREAMER_SOURCES
        src/components/noise_source.cpp
        src/core/noise_fft2.cpp
    )
endif()
if(ENABLE_V4L2_SOURCE)
    list(APPEND VSTREAMER_SOURCES src/components/v4l2_source.cpp)
endif()
if(ENABLE_JPEG_DECODER_MULTICORE)
    list(APPEND VSTREAMER_SOURCES src/components/jpeg_decoder_multicore.cpp)
endif()
if(ENABLE_H264_DECODER_MPP)
    list(APPEND VSTREAMER_SOURCES src/components/h264_decoder_mpp.cpp)
endif()
if(ENABLE_H264_ENCODER_MPP)
    list(APPEND VSTREAMER_SOURCES src/components/h264_encoder_mpp.cpp)
endif()
if(ENABLE_H264_ENCODER_CEDAR)
    list(APPEND VSTREAMER_SOURCES src/components/h264_encoder_cedar.cpp)
endif()
if(ENABLE_H264_ENCODER_INTEL)
    list(APPEND VSTREAMER_SOURCES src/components/h264_encoder_intel.cpp)
endif()
if(ENABLE_MKV_SINK)
    list(APPEND VSTREAMER_SOURCES src/components/mkv_sink.cpp)
endif()
if(ENABLE_SDL_SINK)
    list(APPEND VSTREAMER_SOURCES
        src/components/sdl_nv12_presenter.cpp
        src/components/sdl_sink.cpp
    )
endif()
if(ENABLE_STREAM_SENDER OR ENABLE_STREAM_RECEIVER)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/isal_ec.cmake)
    vstreamer_add_isal_ec()
    list(APPEND VSTREAMER_SOURCES src/core/rs_block_erasure.cpp)
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

set(VSTREAMER_LIBS)
if(ENABLE_JPEG_DECODER_MULTICORE OR ENABLE_H264_ENCODER_CEDAR OR ENABLE_H264_ENCODER_INTEL OR ENABLE_MKV_SINK)
    pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET libavcodec libavutil)
    list(APPEND VSTREAMER_LIBS PkgConfig::LIBAV)
endif()
if(ENABLE_MKV_SINK)
    pkg_check_modules(LIBAVFORMAT REQUIRED IMPORTED_TARGET libavformat)
    list(APPEND VSTREAMER_LIBS PkgConfig::LIBAVFORMAT)
endif()
if(ENABLE_SDL_SINK)
    pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
    list(APPEND VSTREAMER_LIBS PkgConfig::SDL2)
endif()
if(ENABLE_H264_DECODER_MPP OR ENABLE_H264_ENCODER_MPP)
    if(MPP_FOUND)
        list(APPEND VSTREAMER_LIBS PkgConfig::MPP)
    endif()
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/components/components_config.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/components/components_config.hpp
    @ONLY
)

add_library(vstreamer_source ${VSTREAMER_SOURCES})
set(VSTREAMER_LSAN_HOOKS)
if(CMAKE_CXX_FLAGS MATCHES "fsanitize=address")
    add_library(vstreamer_lsan_hooks OBJECT src/core/lsan_default_options.cpp)
    target_compile_options(vstreamer_lsan_hooks PRIVATE -fsanitize=address)
    target_compile_definitions(vstreamer_lsan_hooks PRIVATE
        "VSTREAMER_LSAN_SUPPRESSIONS_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/cmake/lsan_suppressions.txt\"")
    set(VSTREAMER_LSAN_HOOKS vstreamer_lsan_hooks)
    message(STATUS "vstreamer: LeakSanitizer suppressions for SDL/Mesa → cmake/lsan_suppressions.txt")
endif()
target_include_directories(vstreamer_source PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
if(VSTREAMER_LIBS)
    target_link_libraries(vstreamer_source PUBLIC ${VSTREAMER_LIBS})
endif()
target_compile_options(vstreamer_source PRIVATE -Wall -Wextra -Wpedantic)
if(TARGET isal_ec)
    target_link_libraries(vstreamer_source PUBLIC isal_ec)
endif()
if(TARGET PFFFT::PFFFT)
    target_link_libraries(vstreamer_source PUBLIC PFFFT::PFFFT)
endif()
if(ENABLE_NOISE_SOURCE)
    find_package(OpenMP)
    if(OpenMP_CXX_FOUND)
        target_link_libraries(vstreamer_source PUBLIC OpenMP::OpenMP_CXX)
    endif()
endif()

function(vstreamer_add_app name)
    set(options LSAN COMMON)
    cmake_parse_arguments(VSA "${options}" "" "" ${ARGN})
    set(_app_sources ${VSA_UNPARSED_ARGUMENTS})
    add_executable(${name} ${_app_sources})
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
    target_link_libraries(${name} PRIVATE vstreamer_source)
    if(VSA_COMMON)
        target_link_libraries(${name} PRIVATE apps_common)
    endif()
    if(VSA_LSAN)
        target_link_libraries(${name} PRIVATE ${VSTREAMER_LSAN_HOOKS})
    endif()
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
endfunction()

message(STATUS "vstreamer components:")
message(STATUS "  ENABLE_NOISE_SOURCE=${ENABLE_NOISE_SOURCE}")
message(STATUS "  ENABLE_V4L2_SOURCE=${ENABLE_V4L2_SOURCE}")
message(STATUS "  ENABLE_JPEG_DECODER_MULTICORE=${ENABLE_JPEG_DECODER_MULTICORE}")
message(STATUS "  ENABLE_H264_DECODER_MPP=${ENABLE_H264_DECODER_MPP}")
message(STATUS "  ENABLE_H264_ENCODER_MPP=${ENABLE_H264_ENCODER_MPP}")
message(STATUS "  ENABLE_H264_ENCODER_CEDAR=${ENABLE_H264_ENCODER_CEDAR}")
message(STATUS "  ENABLE_H264_ENCODER_INTEL=${ENABLE_H264_ENCODER_INTEL}")
message(STATUS "  ENABLE_MKV_SINK=${ENABLE_MKV_SINK}")
message(STATUS "  ENABLE_SDL_SINK=${ENABLE_SDL_SINK}")
message(STATUS "  ENABLE_STREAM_SENDER=${ENABLE_STREAM_SENDER}")
message(STATUS "  ENABLE_STREAM_RECEIVER=${ENABLE_STREAM_RECEIVER}")
message(STATUS "  ENABLE_RTP_H264_PAY=${ENABLE_RTP_H264_PAY}")
message(STATUS "  ENABLE_RTP_H264_DEPAY=${ENABLE_RTP_H264_DEPAY}")
message(STATUS "  VSTREAMER_BUILD_TESTS=${VSTREAMER_BUILD_TESTS}")

option(ENABLE_TEST_STREAM_SDL "Build stream_sdl_test loopback bench" OFF)
option(ENABLE_TEST_NOISE_STREAM_SDL "Deprecated alias for ENABLE_TEST_STREAM_SDL" OFF)
if(ENABLE_TEST_NOISE_STREAM_SDL)
    set(ENABLE_TEST_STREAM_SDL ON)
endif()

option(ENABLE_TEST_UVC_JPEGDEC_KMSDRM "Deprecated; use stream_sdl --display kmsdrm --source /dev/video0" OFF)
option(ENABLE_TEST_UVC_JPEGDEC_DMKS "Deprecated alias for ENABLE_TEST_UVC_JPEGDEC_KMSDRM" OFF)
if(ENABLE_TEST_UVC_JPEGDEC_DMKS)
    message(WARNING "ENABLE_TEST_UVC_JPEGDEC_DMKS is deprecated; use stream_sdl --display kmsdrm --source /dev/video0")
    set(ENABLE_TEST_UVC_JPEGDEC_KMSDRM ON)
endif()
if(ENABLE_TEST_UVC_JPEGDEC_KMSDRM)
    message(WARNING "ENABLE_TEST_UVC_JPEGDEC_KMSDRM is deprecated; use stream_sdl --display kmsdrm --source /dev/video0")
    set(ENABLE_TEST_STREAM_SDL ON CACHE BOOL "" FORCE)
endif()

if(VSTREAMER_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(src/apps)

if(VSTREAMER_BUILD_TESTS)

    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(googletest)

    add_subdirectory(tests)
endif()
