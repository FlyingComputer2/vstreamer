vstreamer_add_app(rs_block_id_pace_test rs_block_id_pace_test.cpp)

if(VSTREAMER_BUILD_TESTS)
    add_test(NAME rs_block_id_pace_test COMMAND rs_block_id_pace_test)
endif()
