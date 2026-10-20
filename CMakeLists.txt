# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
#
# tbox - a small Telegram command line client in C, built on TDLib.
#
#   cmake -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   ctest --test-dir build
#   cmake --install build --prefix dist      # dist/bin/tbox + libtdjson
cmake_minimum_required(VERSION 3.18)

project(tbox VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON) # POSIX prototypes (dlopen, termios, localtime_r)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Release builds get the tag name (v1.2.3) so `tbox --version` says which one.
set(TBOX_VERSION "${PROJECT_VERSION}" CACHE STRING "Version string printed by tbox --version")

option(TBOX_BUILD_TESTS "Build the unit tests" ON)

if(MSVC)
  add_compile_options(/W4 /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# --- cJSON (vendored, MIT) ---------------------------------------------------
add_library(cjson STATIC third_party/cjson/cJSON.c)
target_include_directories(cjson SYSTEM PUBLIC third_party/cjson)
if(NOT MSVC)
  target_compile_options(cjson PRIVATE -w)
endif()

# --- core: everything a command needs, without main() -----------------------
add_library(tbox_core STATIC
  src/td.c
  src/config.c
  src/auth.c
  src/platform.c
)
target_include_directories(tbox_core PUBLIC src)
target_link_libraries(tbox_core PUBLIC cjson)
target_compile_definitions(tbox_core PUBLIC TBOX_VERSION="${TBOX_VERSION}")
if(UNIX)
  target_link_libraries(tbox_core PUBLIC ${CMAKE_DL_LIBS})
endif()

# --- the TDLib runtime -------------------------
include(cmake/tdjson.cmake)
install(FILES LICENSE THIRD_PARTY_NOTICES.md README.md DESTINATION .)

# --- tests: a fake tdjson, no network, no account ----------------------------
if(TBOX_BUILD_TESTS)
  enable_testing()
  add_library(tbox_fake_td STATIC tests/fake_td.c)
  target_link_libraries(tbox_fake_td PUBLIC tbox_core)

  foreach(t test_td test_auth test_config)
    add_executable(${t} tests/${t}.c)
    target_link_libraries(${t} PRIVATE tbox_fake_td)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
endif()
