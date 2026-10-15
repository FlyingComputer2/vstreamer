# ESP-IDF component: vstreamer core and the portable components (no apps).
# Add <vstreamer>/idf/vstreamer to EXTRA_COMPONENT_DIRS of an IDF project.
set(VSTREAMER_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

set(ENABLE_MKV_SINK ON)
set(ENABLE_STREAM_SENDER ON)
set(ENABLE_STREAM_RECEIVER ON)
set(ENABLE_RTP_H264_PAY ON)
set(ENABLE_RTP_H264_DEPAY ON)

set(VSTREAMER_SOURCES)
include(${VSTREAMER_ROOT}/cmake/vstreamer_sources.cmake)
list(TRANSFORM VSTREAMER_SOURCES PREPEND ${VSTREAMER_ROOT}/)

# ISA-L's portable erasure code; not fetched during IDF's early requirement scan.
set(VSTREAMER_IDF_ISAL_SOURCES)
set(VSTREAMER_IDF_ISAL_INCLUDES)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(VSTREAMER_NEEDS_ISAL)
        include(${VSTREAMER_ROOT}/cmake/isal_ec.cmake)
        vstreamer_fetch_isal()
        set(VSTREAMER_IDF_ISAL_SOURCES ${VSTREAMER_ISAL_ROOT}/erasure_code/ec_base.c)
        set(VSTREAMER_IDF_ISAL_INCLUDES ${VSTREAMER_ISAL_ROOT}/include
                                        ${VSTREAMER_ISAL_ROOT}/erasure_code)
    endif()
    configure_file(${VSTREAMER_ROOT}/src/components/components_config.hpp.in
                   ${CMAKE_CURRENT_BINARY_DIR}/generated/components/components_config.hpp)
endif()

idf_component_register(
    SRCS ${VSTREAMER_SOURCES}
         ${VSTREAMER_IDF_ISAL_SOURCES}
    INCLUDE_DIRS ${VSTREAMER_ROOT}/src
                 ${CMAKE_CURRENT_BINARY_DIR}/generated
    PRIV_INCLUDE_DIRS compat
                      ${VSTREAMER_IDF_ISAL_INCLUDES}
    REQUIRES lwip pthread
    PRIV_REQUIRES vfs
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
)
if(VSTREAMER_IDF_ISAL_SOURCES)
    set_source_files_properties(${VSTREAMER_IDF_ISAL_SOURCES}
        PROPERTIES COMPILE_DEFINITIONS NDEBUG)
endif()
