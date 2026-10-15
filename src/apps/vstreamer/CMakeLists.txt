# Pipeline-file loader and management plane, shared by the app and the tests.
add_library(vstreamer_app STATIC
    pipeline_set.cpp
    mplane.cpp
)
target_include_directories(vstreamer_app PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(vstreamer_app PUBLIC vstreamer_source)
target_compile_options(vstreamer_app PRIVATE -Wall -Wextra -Wpedantic)

vstreamer_add_app(vstreamer main.cpp COMMON LSAN)
target_link_libraries(vstreamer PRIVATE vstreamer_app)
