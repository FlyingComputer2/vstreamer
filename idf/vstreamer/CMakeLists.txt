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
# The P4's hardware H.264 encoder (esp_h264) and MIPI-CSI camera (esp_video); see
# idf_component.yml.
if(IDF_TARGET STREQUAL "esp32p4")
    set(ENABLE_H264_ENCODER_ESP32P4 ON)
    set(ENABLE_ESP32P4_CSI_CAMERA_SRC ON)
    set(ENABLE_ESP32_H264_ISP_SRC ON)
    set(ENABLE_ESP32_H264_ISP_NG_SRC ON)
    list(APPEND VSTREAMER_SOURCES src/components/h264_encoder_esp32p4.cpp
                                  src/components/esp32p4_csi_camera_src.cpp
                                  src/components/esp32_h264_isp_src.cpp
                                  src/components/esp32p4_csi_device.cpp
                                  src/components/esp32_h264_isp_ng_src.cpp)
endif()
list(TRANSFORM VSTREAMER_SOURCES PREPEND ${VSTREAMER_ROOT}/)
# IDF-only helpers (include/vstreamer_idf): the live TX management plane, and live TX itself
# where the camera and hardware encoder exist.
list(APPEND VSTREAMER_SOURCES src/live_mplane.cpp)
if(IDF_TARGET STREQUAL "esp32p4")
    list(APPEND VSTREAMER_SOURCES src/live_tx.cpp)
endif()

# ISA-L's portable erasure code; not fetched during IDF's early requirement scan.
set(VSTREAMER_IDF_ISAL_SOURCES)
set(VSTREAMER_IDF_ISAL_INCLUDES)
# bfc (header-only: shared_sized_buffer); public, component_pdu.hpp includes it.
set(VSTREAMER_IDF_BFC_INCLUDES)
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include(${VSTREAMER_ROOT}/cmake/BfcFetch.cmake)
    set(VSTREAMER_IDF_BFC_INCLUDES ${bfc_SOURCE_DIR}/src)
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
                 include
                 ${CMAKE_CURRENT_BINARY_DIR}/generated
                 ${VSTREAMER_IDF_BFC_INCLUDES}
    PRIV_INCLUDE_DIRS compat
                      ${VSTREAMER_IDF_ISAL_INCLUDES}
    REQUIRES lwip pthread
    PRIV_REQUIRES vfs esp_timer esp_pm
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
    # bfc (stream_sender's queue transport) throws on fatal errors; with IDF's C++ exceptions
    # off a throw aborts.
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
)
if(VSTREAMER_IDF_ISAL_SOURCES)
    set_source_files_properties(${VSTREAMER_IDF_ISAL_SOURCES}
        PROPERTIES COMPILE_DEFINITIONS NDEBUG)
endif()
