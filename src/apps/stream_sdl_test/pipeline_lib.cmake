if(TARGET vstreamer_bench_pipeline)
    return()
endif()

set(_bench_pipeline_dir ${CMAKE_SOURCE_DIR}/src/apps/stream_sdl_test)
set(_bench_core
    ${_bench_pipeline_dir}/pipeline_state.cpp
    ${_bench_pipeline_dir}/diag.cpp
    ${_bench_pipeline_dir}/metrics_sync.cpp
)

if(VSTREAMER_APP_TX_OK AND VSTREAMER_APP_RX_OK)
    add_library(vstreamer_bench_pipeline STATIC
        ${_bench_core}
        ${_bench_pipeline_dir}/bench_stream_metrics.cpp
        ${_bench_pipeline_dir}/bench_stream_status_metrics.cpp
        ${_bench_pipeline_dir}/channel_controller.cpp
        ${_bench_pipeline_dir}/link_emulator.cpp
        ${_bench_pipeline_dir}/bench_console.cpp
    )
elseif(VSTREAMER_APP_TX_OK)
    add_library(vstreamer_bench_pipeline STATIC ${_bench_core})
    target_compile_definitions(vstreamer_bench_pipeline PUBLIC VSTREAMER_BENCH_TX_ONLY=1)
elseif(VSTREAMER_APP_RX_OK)
    add_library(vstreamer_bench_pipeline STATIC
        ${_bench_core}
        ${_bench_pipeline_dir}/bench_stream_status_metrics.cpp
    )
    target_compile_definitions(vstreamer_bench_pipeline PUBLIC VSTREAMER_BENCH_RX_ONLY=1)
else()
    return()
endif()

target_include_directories(vstreamer_bench_pipeline PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(vstreamer_bench_pipeline PUBLIC apps_common vstreamer_source)
target_compile_options(vstreamer_bench_pipeline PRIVATE -Wall -Wextra -Wpedantic)
