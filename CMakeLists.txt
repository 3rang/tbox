# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
#
# tbox - C core (Windows-first; Linux/macOS later).
# Build: powershell -ExecutionPolicy Bypass -File scripts\build-native.ps1
cmake_minimum_required(VERSION 3.24)
project(tboxnative C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(tbox_td STATIC
  src/td/tbox_td.c
  src/td/tbox_td_auth.c
)
target_include_directories(tbox_td PUBLIC src)

add_executable(tbox src/main.c)
target_link_libraries(tbox PRIVATE tbox_td)

enable_testing()

add_executable(test_tbox_td tests/test_tbox_td.c)
target_link_libraries(test_tbox_td PRIVATE tbox_td)

add_executable(test_td_auth tests/test_td_auth.c)
target_link_libraries(test_td_auth PRIVATE tbox_td)
add_test(NAME td_auth_flow COMMAND test_td_auth)

add_test(NAME tbox_td_smoke COMMAND test_tbox_td)

# Vendored TDLib: link the td_json_client C API and stage the DLLs beside
# every binary that uses it. TBOX_TDJSON_TARGETS must be set BEFORE the
# include so the module links and stages for all of them.
set(TBOX_TDJSON_TARGETS tbox test_tbox_td)
include(cmake/tdjson-vendored.cmake)
