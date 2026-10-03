if(NOT VSTREAMER_APP_TX_OK)
    message(FATAL_ERROR "uvc_stream_sender requires VSTREAMER_APP_TX_OK")
endif()

vstreamer_add_app(uvc_stream_sender main.cpp COMMON)
target_link_libraries(uvc_stream_sender PRIVATE vstreamer_bench_pipeline)
