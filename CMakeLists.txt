# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
#
# tbox - C core (Windows-first; Linux/macOS later).
# Build: powershell -ExecutionPolicy Bypass -File scripts\build-native.ps1
cmake_minimum_required(VERSION 3.24)
project(tboxnative C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(tbox_td src/td/tbox_td.c src/td/tbox_td_auth.c)
target_include_directories(tbox_td PUBLIC src)

enable_testing()

add_executable(tbox src/main.c)
target_link_libraries(tbox PRIVATE tbox_td)

add_executable(test_tbox_td tests/test_tbox_td.c)
target_link_libraries(test_tbox_td PRIVATE tbox_td)

add_executable(test_td_auth tests/test_td_auth.c)
target_link_libraries(test_td_auth PRIVATE tbox_td)
add_test(NAME td_auth_flow COMMAND test_td_auth)

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
