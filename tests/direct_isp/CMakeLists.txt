cmake_minimum_required(VERSION 3.16)
project(direct_isp_contract_test LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
get_filename_component(VSTREAMER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(BFC_INCLUDE_DIR "${VSTREAMER_ROOT}/build/_deps/bfc-src/src" CACHE PATH "Directory containing bfc headers")
if(NOT EXISTS "${BFC_INCLUDE_DIR}/bfc/shared_sized_buffer.hpp")
    message(FATAL_ERROR "Set BFC_INCLUDE_DIR to the directory containing bfc/shared_sized_buffer.hpp")
endif()
find_package(Threads REQUIRED)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/components")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/components/components_config.hpp"
    "#pragma once\n#define ENABLE_ESP32_H264_ISP_NG_SRC\n#define ENABLE_ESP32P4_CSI_CAMERA_SRC\n#define ENABLE_H264_ENCODER_ESP32P4\n")
add_executable(direct_isp_contract_test direct_isp_test.cpp
    ${VSTREAMER_ROOT}/src/components/esp32_h264_isp_ng_src.cpp
    ${VSTREAMER_ROOT}/src/components/esp32p4_csi_camera_src.cpp
    ${VSTREAMER_ROOT}/src/core/component_factory.cpp
    ${VSTREAMER_ROOT}/src/core/sdu_caps.cpp
    ${VSTREAMER_ROOT}/src/core/sdu_type.cpp
    ${VSTREAMER_ROOT}/src/core/port_caps.cpp
    ${VSTREAMER_ROOT}/src/core/pdu_wakeup.cpp)
target_include_directories(direct_isp_contract_test PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated" "${VSTREAMER_ROOT}/src" "${BFC_INCLUDE_DIR}")
target_compile_options(direct_isp_contract_test PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter)
target_link_libraries(direct_isp_contract_test PRIVATE Threads::Threads)
enable_testing()
add_test(NAME direct_isp_contract COMMAND direct_isp_contract_test)
