# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
#
# tbox - C core (Windows-first; Linux/macOS later).
# Build: powershell -ExecutionPolicy Bypass -File scripts\build-native.ps1
cmake_minimum_required(VERSION 3.24)
project(tboxnative C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(tbox_td src/tbox_td.c)
target_include_directories(tbox_td PUBLIC src)

enable_testing()

add_executable(test_tbox_td tests/test_tbox_td.c)
target_link_libraries(test_tbox_td PRIVATE tbox_td)

# The smoke test loads the vendored tdjson next to the repo root.
if(WIN32)
    set(TBOX_TD_VENDORED "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tdjson.dll")
else()
    set(TBOX_TD_VENDORED "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libtdjson.so")
endif()
if(EXISTS "${TBOX_TD_VENDORED}")
    add_test(NAME tbox_td_smoke COMMAND test_tbox_td "${TBOX_TD_VENDORED}")
else()
    message(STATUS "tdjson not vendored under vendor/ — smoke test not registered")
endif()
