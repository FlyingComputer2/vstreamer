vstreamer_add_app(rs_fec_test rs_fec_test.cpp)

if(VSTREAMER_BUILD_TESTS)
    add_test(NAME rs_fec_test COMMAND rs_fec_test)
endif()
