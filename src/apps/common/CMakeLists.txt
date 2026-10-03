set(_apps_common_sources
    app_console.cpp
    app_metrics.cpp
    cpu_map.cpp
    pipeline_controller.cpp
    queues.cpp
    stage_latency.cpp
)

if(ENABLE_V4L2_SOURCE AND ENABLE_NOISE_SOURCE)
    list(APPEND _apps_common_sources tx/source_selector.cpp)
endif()

add_library(apps_common STATIC ${_apps_common_sources})
target_include_directories(apps_common
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(apps_common PUBLIC vstreamer_source)
target_compile_options(apps_common PRIVATE -Wall -Wextra -Wpedantic)
