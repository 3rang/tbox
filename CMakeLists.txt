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
# third_party/qrcodegen. Used by util/qrprint.c and demo/qr_login.c.
# Re-vendor with scripts/fetch-qrcodegen.ps1.
add_library(qrcodegen STATIC third_party/qrcodegen/qrcodegen.c)
target_include_directories(qrcodegen PUBLIC third_party/qrcodegen)

# ---- compiler hardening (ground rule 1: /W4 /WX green) ----------------------
# Applied to project code only - vendored deps (cjson/qrcodegen) and the
# proven demo binary (ported wholesale in Step 4) build without it for now.
function(tbox_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

# Telegram credentials (from https://my.telegram.org/apps) are NOT part
# of the build: nothing is configured, cached or compiled in here.
# `tbox auth` (and the qr-login demo) read them from the environment at
# run time, so one binary is enough:
#   Windows : set TG_API_ID=<id> && set TG_API_HASH=<hash>
#   Linux   : export TG_API_ID=<id> TG_API_HASH=<hash>

# ---- libtdjson availability --------------------------------------------------
# Auto-detect the vendored prebuilt library per OS (same rule as
# cmake/tdjson-vendored.cmake; keep both in sync). Everything that needs
# TDLib (tbox's auth layer + the qr-login demo) builds only when found;
# a missing lib is a clear message, not a broken configure.
#
#   (unset)                          auto-detect vendor/<lib>
#   -DTBOX_TDJSON_LIBRARY=OFF        force the core-only build (CI uses this
#                                    to prove the no-TDLib path still works)
#   -DTBOX_TDJSON_LIBRARY=<path>     use exactly this library
if(TBOX_TDJSON_LIBRARY STREQUAL "OFF")
    set(TBOX_TDJSON_LIBRARY "")
elseif(NOT TBOX_TDJSON_LIBRARY)
    if(WIN32)
        set(_tbox_tdjson_probe "${CMAKE_SOURCE_DIR}/vendor/tdjson.dll")
    elseif(APPLE)
        set(_tbox_tdjson_probe "${CMAKE_SOURCE_DIR}/vendor/libtdjson.dylib")
    else()
        set(_tbox_tdjson_probe "${CMAKE_SOURCE_DIR}/vendor/libtdjson.so")
    endif()
    if(EXISTS "${_tbox_tdjson_probe}")
        set(TBOX_TDJSON_LIBRARY "${_tbox_tdjson_probe}")
    endif()
endif()

# ---- core library: tboxcore ------------------------------------------------
# CLI dispatcher + internal command registry + interrupt listener + thread
# wrapper + console utils. Shared by tbox.exe and the headless unit tests.
# TDLib-free by rule: src/td/ compiles into the tbox binary only (below),
# and cmd/cmd.c receives the auth runner as an injected function pointer.
# src/ is on the include path so internal headers resolve as:
#   #include "cli/cli.h"      -> src/cli/cli.h
#   #include "cmd/cmd.h"      -> src/cmd/cmd.h
#   #include "core/index.h"   -> src/core/index.h
#   #include "util/tbox_signal.h" -> src/util/tbox_signal.h
#   #include "td/tdhandler.h" -> src/td/tdhandler.h
# The FTP transport lives in src/net/: only sockets, never Telegram, so it is
# part of tboxcore and is exercised by tests/test_ftpd.c over loopback.
add_library(tboxcore STATIC
    src/cli/cli.c
    src/cmd/args.c
    src/cmd/cmd.c
    src/cmd/selftest.c
    src/cmd/serve.c
    src/core/index.c
    src/core/indexfile.c
    src/core/read.c
    src/core/scanmap.c
    src/net/ftp_proto.c
    src/net/ftpd.c
    src/util/datadir.c
    src/util/lockfile.c
    src/util/statusfile.c)
target_include_directories(tboxcore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cli
    ${CMAKE_CURRENT_SOURCE_DIR}/src/cmd
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/src/util)
# Interrupt listener: one header (util/tbox_signal.h), one native backend per OS.
# Thread wrapper: one header (util/tbox_thread.h), one native backend per OS.
# tests/test_signal.c + tests/test_thread.c are the SAME files on both platforms.
if(WIN32)
    target_sources(tboxcore PRIVATE src/util/signal_win.c src/util/thread_win.c)
    # BCryptGenRandom: the FTP session token's entropy source.
    # ws2_32: Winsock, for the FTP transport and its tests.
    target_link_libraries(tboxcore PRIVATE bcrypt ws2_32)
else()
    target_sources(tboxcore PRIVATE src/util/signal_posix.c src/util/thread_posix.c)
    find_package(Threads REQUIRED)          # -> -pthread (compile + link)
    target_link_libraries(tboxcore PRIVATE Threads::Threads)
endif()
# Console utils (portable APIs, internal #ifdefs): VT mode + masked input,
# and terminal QR rendering (its only third-party dep is qrcodegen).
target_sources(tboxcore PRIVATE src/util/term.c src/util/qrprint.c)
# The archive index parses caption JSON, so tboxcore needs cJSON. TDLib-free
# is still the rule: core/ and util/ never include td_json_client.h.
target_link_libraries(tboxcore PUBLIC qrcodegen cjson)
tbox_apply_warnings(tboxcore)

# ---- main binary: tbox ----------------------------------------------------
add_executable(tbox src/main.c)
target_link_libraries(tbox PRIVATE tboxcore cjson)
target_include_directories(tbox PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tdlib_headers)
target_compile_definitions(tbox PRIVATE TBOX_VERSION="0.5.0")

# TD auth layer: state machine (tdhandler) + client config (tdparams).
# Compiled into the binary (never into tboxcore) and only when libtdjson
# is vendored; without it main.c leaves the runner un-injected and
# `tbox auth` reports itself unavailable (see cmd/cmd.c).
if(TBOX_TDJSON_LIBRARY)
    target_sources(tbox PRIVATE
        src/td/tdhandler.c
        src/td/tdparams.c
        src/td/tdbridge.c
        src/td/tdscan.c
        src/td/tdserve.c)
    target_compile_definitions(tbox PRIVATE TBOX_WITH_TDLIB=1)
endif()
tbox_apply_warnings(tbox)

# ---- demo: qr-login (built whenever TDLib is available) --------------------
# demo/qr_login.c — minimal TDLib QR-code login demo (new API, cJSON,
# qrcodegen ASCII QR render). Kept as a permanent regression proof of the
# auth flow. Credentials come from the environment (TG_API_ID /
# TG_API_HASH), exactly like `tbox auth`.
# The demo never sends a phone/SMS/code — QR authentication only.
if(TBOX_TDJSON_LIBRARY)
    add_executable(qr-login demo/qr_login.c)
    target_link_libraries(qr-login PRIVATE cjson qrcodegen)
    target_include_directories(qr-login PRIVATE third_party/tdlib_headers)
else()
    message(STATUS
        "libtdjson not found in vendor/ - skipping TD auth + qr-login demo. "
        "Run scripts/fetch-tdjson.ps1 (Windows) or scripts/fetch-tdjson.sh "
        "(Linux/macOS); tbox core CLI + tests still build.")
endif()

# ---- tdjson: link + stage DLLs next to the binaries that need them ----------
# (see cmake/tdjson-vendored.cmake) tbox + qr-login only — the unit tests
# must stay TDLib-free, so they are NOT listed here. Both targets are
# defined above before this include.
if(TBOX_TDJSON_LIBRARY)
    set(TBOX_TDJSON_TARGETS tbox qr-login)
    include(cmake/tdjson-vendored.cmake)
endif()

# ---- tests: every tests/test_*.c -> test_*.exe + CTest entry ---------------
# Headless by rule (no TDLib, no network). See tests/CMakeLists.txt.
#
# Switch tests OFF for faster iteration while hacking on the CLI:
#   cmake -S . -B build -DTBOX_BUILD_TESTS=OFF
#   scripts\clean-configure.ps1 -NoTests
# New tests/test_*.c are picked up automatically while ON (CONFIGURE_DEPENDS).
option(TBOX_BUILD_TESTS "Build unit test binaries (tests/test_*.exe) + CTest" ON)
if(TBOX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
