# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Tarang Patel

cmake_minimum_required(VERSION 3.22)
project(tbox C)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---- vendored third-party libraries ---------------------------------------
# cJSON — single-file JSON parser (MIT) vendored in third_party/cjson.
# Keep up to date from https://github.com/DaveGamble/cJSON (currently v1.7.19).
add_library(cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cjson)

# qrcodegen — Nayuki QR-Code-generator C port (MIT) vendored in
# third_party/qrcodegen. Used only by demo/qr_login.c. Re-vendor with
# scripts/fetch-qrcodegen.ps1.
add_library(qrcodegen STATIC third_party/qrcodegen/qrcodegen.c)
target_include_directories(qrcodegen PUBLIC third_party/qrcodegen)

# ---- core library: tboxcore ------------------------------------------------
# CLI dispatcher + internal command registry. Shared by tbox.exe and the
# headless unit tests. src/ is on the include path so internal headers
# resolve as:
#   #include "cli/cli.h"      -> src/cli/cli.h
#   #include "cmd/cmd.h"      -> src/cmd/cmd.h
#   #include "td/tdhandler.h" -> src/td/tdhandler.h
add_library(tboxcore STATIC src/cli/cli.c src/cmd/cmd.c)
target_include_directories(tboxcore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cli
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cmd)

# ---- main binary: tbox ----------------------------------------------------
add_executable(tbox src/main.c)
target_link_libraries(tbox PRIVATE tboxcore cjson)
target_include_directories(tbox PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tdlib_headers)
target_compile_definitions(tbox PRIVATE TBOX_VERSION="0.5.0")

# ---- demo: qr-login (always built) -----------------------------------------
# demo/qr_login.c — minimal TDLib QR-code login demo (new API, cJSON,
# qrcodegen ASCII QR render). Built together with tbox.exe so the proven
# auth flow stays a permanent regression proof. Credentials are compiled in,
# easy-telegram style (they live only in the gitignored build/ cache, never
# in source):
#   cmake -S . -B build -DTG_API_ID=<id> -DTG_API_HASH=<hash>
# The demo never sends a phone/SMS/code — QR authentication only.
set(TG_API_ID "" CACHE STRING "Telegram api_id (from https://my.telegram.org/apps)")
set(TG_API_HASH "" CACHE STRING "Telegram api_hash (from https://my.telegram.org/apps)")

add_executable(qr-login demo/qr_login.c)
target_link_libraries(qr-login PRIVATE cjson qrcodegen)
target_include_directories(qr-login PRIVATE third_party/tdlib_headers)
if(TG_API_ID AND TG_API_HASH)
  target_compile_definitions(qr-login PRIVATE
    TBOX_TG_API_ID=${TG_API_ID}
    TBOX_TG_API_HASH="${TG_API_HASH}")
else()
  message(WARNING
    "qr-login demo built without Telegram credentials. Re-configure with "
    "-DTG_API_ID=<id> -DTG_API_HASH=<hash> to enable the QR flow.")
  target_compile_definitions(qr-login PRIVATE TBOX_TG_API_ID=0)
endif()

# ---- tests: every tests/test_*.c -> test_*.exe + CTest entry ---------------
# Headless by rule (no TDLib, no network). See tests/CMakeLists.txt.
enable_testing()
add_subdirectory(tests)

# ---- tdjson: link + stage DLLs next to the binaries that need them ----------
# Targets that link libtdjson and need it staged next to them
# (see cmake/tdjson-vendored.cmake). tbox + qr-login only — the unit tests
# must stay TDLib-free, so they are NOT listed here.
set(TBOX_TDJSON_TARGETS tbox qr-login)
include(cmake/tdjson-vendored.cmake)