include(${CMAKE_CURRENT_SOURCE_DIR}/app_capabilities.cmake)

set(_apps_common_sources
    app_console.cpp
    app_metrics.cpp
    cpu_map.cpp
    pipeline_controller.cpp
    queues.cpp
    stage_latency.cpp
)

if(VSTREAMER_APP_TX_OK)
    list(APPEND _apps_common_sources tx/tx_console.cpp)
endif()
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

if(VSTREAMER_APP_TX_OK)
    target_compile_definitions(apps_common PUBLIC VSTREAMER_APP_TX_OK=1)
endif()
if(VSTREAMER_APP_RX_OK)
    target_compile_definitions(apps_common PUBLIC VSTREAMER_APP_RX_OK=1)
endif()
