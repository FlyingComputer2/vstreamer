# Management plane handler, shared by the app and the tests.
add_library(channel_sim_lib STATIC channel_mplane.cpp)
target_include_directories(channel_sim_lib PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(channel_sim_lib PUBLIC apps_common)
target_compile_options(channel_sim_lib PRIVATE -Wall -Wextra -Wpedantic)

vstreamer_add_app(channel_sim main.cpp COMMON)
target_link_libraries(channel_sim PRIVATE channel_sim_lib)
