if(NOT VSTREAMER_APP_RX_OK)
    message(FATAL_ERROR "sdl_stream_receiver requires VSTREAMER_APP_RX_OK")
endif()

vstreamer_add_app(sdl_stream_receiver main.cpp COMMON)
target_link_libraries(sdl_stream_receiver PRIVATE vstreamer_bench_pipeline)
