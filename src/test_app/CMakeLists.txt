if(ENABLE_NOISE_SOURCE)
    add_subdirectory(noise_fft_bench)
endif()

if(ENABLE_STREAM_SENDER OR ENABLE_STREAM_RECEIVER)
    add_subdirectory(rs_fec_test)
    add_subdirectory(rs_block_id_pace_test)
endif()

