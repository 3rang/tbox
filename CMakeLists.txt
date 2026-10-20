# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
#
# tbox - C core (Windows-first; Linux/macOS later).
# Build: powershell -ExecutionPolicy Bypass -File scripts\build-native.ps1
cmake_minimum_required(VERSION 3.24)
project(tboxnative C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# cJSON (vendored, MIT) — JSON parse/build for events and captions.
add_library(cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cjson)

# tbox_td_core - auth FSM + client loop + the UI-free session engine.
# Injectable TBoxTdApi: no native references, so its tests run without the
# real dll (CI-friendly).
add_library(tbox_td_core STATIC
  src/td/tbox_td_auth.c
  src/td/tbox_td_client.c
  src/service/tbox_session.c
)
target_include_directories(tbox_td_core PUBLIC src)
target_link_libraries(tbox_td_core PUBLIC cjson)

# tbox_td — real tdjson bindings. The only library allowed to touch the
# native symbols; linked through the import lib from tdjson-vendored.cmake.
add_library(tbox_td STATIC
  src/td/tbox_td.c
  src/td/tbox_td_api.c
)
target_include_directories(tbox_td PUBLIC src)

add_executable(tbox
  src/main.c
  src/cli/cli.c
  src/cmd/cmd.c
)
target_link_libraries(tbox PRIVATE tbox_td_core tbox_td)

enable_testing()

# real-runtime smoke test (needs the staged tdjson.dll)
add_executable(test_tbox_td tests/test_tbox_td.c)
target_link_libraries(test_tbox_td PRIVATE tbox_td_core tbox_td)
add_test(NAME tbox_td_smoke COMMAND test_tbox_td)

add_executable(test_td_auth tests/test_td_auth.c)
target_link_libraries(test_td_auth PRIVATE tbox_td_core)
add_test(NAME td_auth_flow COMMAND test_td_auth)

add_executable(test_td_client tests/test_td_client.c)
target_link_libraries(test_td_client PRIVATE tbox_td_core)
add_test(NAME td_client_flow COMMAND test_td_client)

add_executable(test_session tests/test_session.c)
target_link_libraries(test_session PRIVATE tbox_td_core)
add_test(NAME session_flow COMMAND test_session)

# Vendored TDLib: link the td_json_client C API and stage the DLLs beside
# every binary that uses it. TBOX_TDJSON_TARGETS must be set BEFORE the
# include so the module links and stages for all of them.
set(TBOX_TDJSON_TARGETS tbox test_tbox_td)
include(cmake/tdjson-vendored.cmake)
