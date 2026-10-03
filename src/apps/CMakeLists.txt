include(${CMAKE_CURRENT_SOURCE_DIR}/common/app_capabilities.cmake)

add_subdirectory(common)
if(VSTREAMER_APP_TX_OK OR VSTREAMER_APP_RX_OK)
    include(${CMAKE_SOURCE_DIR}/src/apps/stream_sdl_test/pipeline_lib.cmake)
endif()

option(ENABLE_APP_UVC_STREAM_SENDER "Build uvc_stream_sender" ON)
option(ENABLE_APP_SDL_STREAM_RECEIVER "Build sdl_stream_receiver" ON)

if(ENABLE_APP_UVC_STREAM_SENDER AND NOT VSTREAMER_APP_TX_OK)
    message(STATUS "ENABLE_APP_UVC_STREAM_SENDER=OFF (TX component set incomplete)")
    set(ENABLE_APP_UVC_STREAM_SENDER OFF)
endif()
if(ENABLE_APP_SDL_STREAM_RECEIVER AND NOT VSTREAMER_APP_RX_OK)
    message(STATUS "ENABLE_APP_SDL_STREAM_RECEIVER=OFF (RX component set incomplete)")
    set(ENABLE_APP_SDL_STREAM_RECEIVER OFF)
endif()

if(ENABLE_APP_UVC_STREAM_SENDER)
    add_subdirectory(uvc_stream_sender)
endif()
if(ENABLE_APP_SDL_STREAM_RECEIVER)
    add_subdirectory(sdl_stream_receiver)
endif()

if(ENABLE_TEST_STREAM_SDL)
    add_subdirectory(stream_sdl_test)
endif()

if(ENABLE_NOISE_SOURCE)
    add_subdirectory(noise_fft_bench_test)
endif()

if(ENABLE_STREAM_SENDER OR ENABLE_STREAM_RECEIVER)
    add_subdirectory(rs_fec_test)
    add_subdirectory(rs_block_id_pace_test)
endif()
