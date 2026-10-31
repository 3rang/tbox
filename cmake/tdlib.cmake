# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
#
# tdlib.cmake - TDLib for tbox, linked at compile time. Defines the target
# `tdlib` that tbox links, and TBOX_TDJSON_FILES (shared libraries to put
# next to the executable; empty for a static build).
#
# Two ways to get it:
#
#   (default)  prebuilt libtdjson for this OS/CPU, downloaded once into the
#              build tree from the tdlib.native packages (sha256 checked),
#              or taken from -DTBOX_TDJSON_DIR=<dir>. Fast. tbox then needs
#              libtdjson beside it at run time; the build and the install
#              rules put it there.
#
#   -DTBOX_TDLIB_FROM_SOURCE=ON
#              fetch TDLib's sources at the pinned commit and build them
#              into tbox statically: one self-contained executable, no
#              shared library. Needs a C++17 compiler, OpenSSL, zlib and
#              gperf, and 10-40 minutes depending on the machine.

set(TBOX_TDLIB_VERSION "1.8.67" CACHE STRING "TDLib version")
set(TBOX_TDLIB_COMMIT "bc9c263e2bfee06aaab41e82db51a103376030bc" CACHE STRING "tdlib/td commit of that version")
option(TBOX_TDLIB_FROM_SOURCE "Build TDLib from source and link it statically" OFF)
set(TBOX_TDJSON_DIR "" CACHE PATH "Directory with a prebuilt libtdjson (empty = download)")

set(TBOX_TDJSON_FILES "")

# =============================================================================
if(TBOX_TDLIB_FROM_SOURCE)
  enable_language(CXX)
  include(FetchContent)
  message(STATUS "tdlib: building ${TBOX_TDLIB_VERSION} from source (this takes a while)")
  # Only the library: no TDLib test suite in ctest, nothing of TDLib's in
  # `cmake --install`.
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(TD_INSTALL_STATIC_LIBRARIES OFF CACHE BOOL "" FORCE)
  set(TD_INSTALL_SHARED_LIBRARIES OFF CACHE BOOL "" FORCE)
  set(TD_ENABLE_LTO OFF CACHE BOOL "" FORCE)
  set(TD_ENABLE_JNI OFF CACHE BOOL "" FORCE)
  set(TD_ENABLE_DOTNET OFF CACHE BOOL "" FORCE)
  # EXCLUDE_FROM_ALL: build only what tbox links, not every TDLib target.
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
    FetchContent_Declare(td
      GIT_REPOSITORY https://github.com/tdlib/td.git
      GIT_TAG ${TBOX_TDLIB_COMMIT}
      EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(td)
  else()
    FetchContent_Declare(td
      GIT_REPOSITORY https://github.com/tdlib/td.git
      GIT_TAG ${TBOX_TDLIB_COMMIT})
    FetchContent_GetProperties(td)
    if(NOT td_POPULATED)
      FetchContent_Populate(td)
      add_subdirectory(${td_SOURCE_DIR} ${td_BINARY_DIR} EXCLUDE_FROM_ALL)
    endif()
  endif()

  add_library(tdlib INTERFACE)
  target_link_libraries(tdlib INTERFACE tdjson_static)
  return()
endif()

# =============================================================================
# Prebuilt: find or download.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  set(_arch "arm64")
else()
  set(_arch "x64")
endif()
if(WIN32)
  set(_rid "win-${_arch}")
elseif(APPLE)
  set(_rid "osx-${_arch}")
else()
  set(_rid "linux-${_arch}")
endif()

set(_sha_linux-x64   789b5f37bafdc2c59b4d355615d3d33db14e9d3e1c02670ed0b1cf8b999e519b)
set(_sha_linux-arm64 4068c135e2d9145aa2ca5e9db9bdedc23f68d0e020dc1e4c5625b8b8075fb4bc)
set(_sha_osx-x64     97d08b28efbf0a358fe23582f1f1b7516802c017cac957f01776f43ecf8bf05d)
set(_sha_osx-arm64   a9e0ff562fb2372fcea626a6de219aab89b4e26745bc427b4237bca965317368)
set(_sha_win-x64     7710da33d22407f536dd3b04d2fa3686a80957d766b9acc2c06a407c99c3756e)
set(_sha_win-arm64   d2b32c5a2171a320c6b229254d81d07d2d09bd79e2e405c697e0e1a50e4490f4)

if(TBOX_TDJSON_DIR STREQUAL "")
  set(_pkg "tdlib.native.${_rid}.${TBOX_TDLIB_VERSION}.nupkg")
  set(_url "https://github.com/ForNeVeR/tdlib.native/releases/download/v${TBOX_TDLIB_VERSION}/${_pkg}")
  set(_dl_dir "${CMAKE_BINARY_DIR}/tdjson")
  set(_native "${_dl_dir}/runtimes/${_rid}/native")

  if(NOT IS_DIRECTORY "${_native}")
    message(STATUS "tdlib: downloading ${_url}")
    file(DOWNLOAD "${_url}" "${_dl_dir}/${_pkg}"
      EXPECTED_HASH SHA256=${_sha_${_rid}}
      SHOW_PROGRESS STATUS _st)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
      list(GET _st 1 _msg)
      message(FATAL_ERROR "tdlib: download failed: ${_msg}\n"
        "Pass -DTBOX_TDJSON_DIR=<dir containing libtdjson>, or build it with "
        "-DTBOX_TDLIB_FROM_SOURCE=ON.")
    endif()
    # A .nupkg is a zip; only the native/ folder matters.
    file(ARCHIVE_EXTRACT INPUT "${_dl_dir}/${_pkg}" DESTINATION "${_dl_dir}" PATTERNS "runtimes/*")
    file(REMOVE "${_dl_dir}/${_pkg}")
  endif()
  set(TBOX_TDJSON_DIR "${_native}")
endif()

if(WIN32)
  set(_lib_name "tdjson.dll")
elseif(APPLE)
  set(_lib_name "libtdjson.dylib")
else()
  set(_lib_name "libtdjson.so")
endif()
set(_shared "${TBOX_TDJSON_DIR}/${_lib_name}")
if(NOT EXISTS "${_shared}")
  message(FATAL_ERROR "tdlib: ${_lib_name} not found in ${TBOX_TDJSON_DIR}")
endif()
message(STATUS "tdlib: prebuilt ${_shared}")

# Everything in the folder goes beside tbox (on Windows also OpenSSL/zlib).
file(GLOB TBOX_TDJSON_FILES
  "${TBOX_TDJSON_DIR}/*.dll" "${TBOX_TDJSON_DIR}/*.so*" "${TBOX_TDJSON_DIR}/*.dylib")

add_library(tdlib INTERFACE)

if(MSVC)
  # A DLL links through an import library, which the package does not
  # ship. Make one from the exported C API with lib.exe.
  set(_def "${CMAKE_BINARY_DIR}/tdjson.def")
  set(_implib "${CMAKE_BINARY_DIR}/tdjson.lib")
  if(NOT EXISTS "${_implib}")
    file(WRITE "${_def}" "LIBRARY tdjson\nEXPORTS\n")
    foreach(_sym td_create_client_id td_send td_receive td_execute
                 td_json_client_create td_json_client_destroy td_json_client_send
                 td_json_client_receive td_json_client_execute
                 td_set_log_verbosity_level td_set_log_message_callback)
      file(APPEND "${_def}" "    ${_sym}\n")
    endforeach()
    if(_arch STREQUAL "arm64")
      set(_machine ARM64)
    else()
      set(_machine X64)
    endif()
    execute_process(
      COMMAND "${CMAKE_AR}" /nologo /machine:${_machine} "/def:${_def}" "/out:${_implib}"
      RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _out)
    if(NOT _rc EQUAL 0 OR NOT EXISTS "${_implib}")
      message(FATAL_ERROR "tdlib: could not create tdjson.lib with ${CMAKE_AR}: ${_out}")
    endif()
  endif()
  target_link_libraries(tdlib INTERFACE "${_implib}")
elseif(APPLE)
  # Link a private copy whose install name is @rpath-relative, so the
  # executable looks for libtdjson.dylib next to itself.
  set(_copy "${CMAKE_BINARY_DIR}/${_lib_name}")
  if(NOT EXISTS "${_copy}")
    file(COPY "${_shared}" DESTINATION "${CMAKE_BINARY_DIR}")
    execute_process(COMMAND install_name_tool -id "@rpath/${_lib_name}" "${_copy}")
  endif()
  list(TRANSFORM TBOX_TDJSON_FILES REPLACE "^${_shared}$" "${_copy}")
  target_link_libraries(tdlib INTERFACE "${_copy}")
  set(CMAKE_BUILD_RPATH "@loader_path")
  set(CMAKE_INSTALL_RPATH "@loader_path")
else()
  # The library's soname is libtdjson.so.<version>, and that is the name
  # the dynamic loader will look for beside tbox; stage a copy under it.
  set(_copy "${CMAKE_BINARY_DIR}/libtdjson.so.${TBOX_TDLIB_VERSION}")
  if(NOT EXISTS "${_copy}")
    configure_file("${_shared}" "${_copy}" COPYONLY)
  endif()
  list(TRANSFORM TBOX_TDJSON_FILES REPLACE "^${_shared}$" "${_copy}")
  target_link_libraries(tdlib INTERFACE "${_copy}")
  set(CMAKE_BUILD_RPATH "$ORIGIN")
  set(CMAKE_INSTALL_RPATH "$ORIGIN")
endif()
