if(NOT TARGET vstreamer_bench_pipeline)
    set(_bench_pipeline_dir ${CMAKE_SOURCE_DIR}/src/test_app/stream_sdl)
    add_library(vstreamer_bench_pipeline STATIC
        ${_bench_pipeline_dir}/pipeline_state.cpp
        ${_bench_pipeline_dir}/diag.cpp
        ${_bench_pipeline_dir}/metrics_sync.cpp
        ${_bench_pipeline_dir}/stages.cpp
        ${_bench_pipeline_dir}/channel_controller.cpp
        ${_bench_pipeline_dir}/link_emulator.cpp
        ${_bench_pipeline_dir}/bench_console.cpp
    )
    target_include_directories(vstreamer_bench_pipeline PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(vstreamer_bench_pipeline PUBLIC apps_common vstreamer_source)
    target_compile_options(vstreamer_bench_pipeline PRIVATE -Wall -Wextra -Wpedantic)
endif()
