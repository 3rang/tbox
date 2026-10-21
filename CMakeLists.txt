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

# tbox_util_log — the engine's log file: one-line JSON records
# (spec 9.5), every text redacted by log_redact() before it is written (S6),
# rotated by size at 5 MiB keeping 3 files. Depends only on cjson, so its
# tests run without tdjson (CI-friendly).
add_library(tbox_util_log STATIC src/util/log.c)
target_include_directories(tbox_util_log PUBLIC src)
target_link_libraries(tbox_util_log PUBLIC cjson)
if(MSVC)
  target_compile_options(tbox_util_log PRIVATE /W4)
endif()

add_executable(test_tbox_log tests/test_log.c)
target_link_libraries(test_tbox_log PRIVATE tbox_util_log)
add_test(NAME util_log COMMAND test_tbox_log)
if(MSVC)
  target_compile_options(test_tbox_log PRIVATE /W4)
endif()

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

# The engine executable. It is named tbox-engine so that `tbox` stays free
# for the GUI app (and the pip console script); the front finds it by name.
add_executable(tbox-engine
  src/main.c
  src/cli/cli.c
  src/cmd/cmd.c
)
target_link_libraries(tbox-engine PRIVATE tbox_td_core tbox_td)

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
set(TBOX_TDJSON_TARGETS tbox-engine test_tbox_td)
include(cmake/tdjson-vendored.cmake)
