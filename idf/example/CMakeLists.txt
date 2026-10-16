# Minimal ESP32-P4 project that builds the vstreamer component.
# Build: idf.py set-target esp32p4 build
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../vstreamer)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(vstreamer-idf-example)
