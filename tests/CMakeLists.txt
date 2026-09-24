# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Tarang Patel
#
# Unit tests: every tests/test_*.c compiles to its own test_*.exe and is
# registered with CTest, so `ctest` runs them all:
#   cmake --build build && ctest --test-dir build -C Release --output-on-failure
# Tests are headless by rule — no TDLib, no network, no secrets.

file(GLOB TBOX_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/test_*.c")
list(SORT TBOX_TEST_SOURCES)

foreach(_test_src ${TBOX_TEST_SOURCES})
    get_filename_component(_test_name "${_test_src}" NAME_WE)   # test_smoke.c -> test_smoke.exe
    add_executable(${_test_name} ${_test_src})
    target_include_directories(${_test_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src                 # src/cli.h, src/td/*.h, ...
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/cjson)
    target_link_libraries(${_test_name} PRIVATE cjson)
    target_compile_definitions(${_test_name} PRIVATE TBOX_VERSION="0.5.0")
    add_test(NAME ${_test_name} COMMAND ${_test_name})
endforeach()