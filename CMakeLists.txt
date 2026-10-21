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

# tbox_util_fsx — filesystem primitives: UTF-8 paths through the Windows
# W functions (coding rules), recursive mkdir, the spec 9.2 durable
# replace and the spec 9.3 single-instance engine.lock. No tdjson, so
# its tests run everywhere (CI-friendly).
add_library(tbox_util_fsx STATIC src/util/fsx.c)
target_include_directories(tbox_util_fsx PUBLIC src)
if(MSVC)
  target_compile_options(tbox_util_fsx PRIVATE /W4)
endif()

add_executable(test_tbox_fsx tests/test_fsx.c)
target_link_libraries(test_tbox_fsx PRIVATE tbox_util_fsx)
add_test(NAME util_fsx COMMAND test_tbox_fsx)
if(MSVC)
  target_compile_options(test_tbox_fsx PRIVATE /W4)
endif()

# tbox_util_rand — OS CSPRNG for object/op IDs and the DB key (spec 3.2):
# BCryptGenRandom (bcrypt.lib) on Windows, getrandom//dev/urandom on
# POSIX. Never rand(). CI-friendly like fsx.
add_library(tbox_util_rand STATIC src/util/rand.c)
target_include_directories(tbox_util_rand PUBLIC src)
if(WIN32)
  target_link_libraries(tbox_util_rand PUBLIC bcrypt)
endif()
if(MSVC)
  target_compile_options(tbox_util_rand PRIVATE /W4)
endif()

add_executable(test_tbox_rand tests/test_rand.c)
target_link_libraries(test_tbox_rand PRIVATE tbox_util_rand)
add_test(NAME util_rand COMMAND test_tbox_rand)
if(MSVC)
  target_compile_options(test_tbox_rand PRIVATE /W4)
endif()

# tbox_util_log — the engine's log file: one-line JSON records
# (spec 9.5), every text redacted by log_redact() before it is written (S6),
# rotated by size at 5 MiB keeping 3 files. Depends only on cjson and
# tbox_util_fsx, so its tests run without tdjson (CI-friendly).
add_library(tbox_util_log STATIC src/util/log.c)
target_include_directories(tbox_util_log PUBLIC src)
target_link_libraries(tbox_util_log PUBLIC cjson tbox_util_fsx)
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
target_link_libraries(tbox_td_core PUBLIC cjson tbox_util_fsx)

# tbox_td — real tdjson bindings. The only library allowed to touch the
# native symbols; linked through the import lib from tdjson-vendored.cmake.
add_library(tbox_td STATIC
  src/td/tbox_td.c
  src/td/tbox_td_api.c
)
target_include_directories(tbox_td PUBLIC src)

# tbox_engine — the loop + protocol + stdio transport (spec 3.1). No native
# TDLib symbols: the api table is injected, so its tests run without the
# dll (CI-friendly). Depends on the session, cJSON and the util layer.
add_library(tbox_engine STATIC
  src/engine/protocol.c
  src/engine/engine.c
  src/engine/transport_stdio.c
)
target_include_directories(tbox_engine PUBLIC src)
target_link_libraries(tbox_engine PUBLIC tbox_td_core cjson tbox_util_log)
if(MSVC)
  target_compile_options(tbox_engine PRIVATE /W4)
endif()

# The engine executable. It is named tbox-engine so that `tbox` stays free
# for the GUI app (and the pip console script); the front finds it by name.
add_executable(tbox-engine
  src/main.c
  src/cli/cli.c
  src/cmd/cmd.c
  src/cmd/serve.c
)
target_link_libraries(tbox-engine PRIVATE tbox_td_core tbox_td tbox_engine)

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

add_executable(test_protocol tests/test_protocol.c)
target_link_libraries(test_protocol PRIVATE tbox_engine)
add_test(NAME engine_protocol COMMAND test_protocol)
if(MSVC)
  target_compile_options(test_protocol PRIVATE /W4)
endif()

add_executable(test_engine tests/test_engine.c)
target_link_libraries(test_engine PRIVATE tbox_engine)
add_test(NAME engine_loop COMMAND test_engine)
if(MSVC)
  target_compile_options(test_engine PRIVATE /W4)
endif()

# P1 guard (spec 5.1): secrets must never reach argv. A source grep, run as
# a test so it fails the build rather than a review.
if(WIN32)
  add_test(NAME no_argv_secrets
    COMMAND powershell -ExecutionPolicy Bypass -File
            "${CMAKE_SOURCE_DIR}/scripts/check-no-argv-secrets.ps1")
endif()

# Vendored TDLib: link the td_json_client C API and stage the DLLs beside
# every binary that uses it. TBOX_TDJSON_TARGETS must be set BEFORE the
# include so the module links and stages for all of them.
set(TBOX_TDJSON_TARGETS tbox-engine test_tbox_td)
include(cmake/tdjson-vendored.cmake)
