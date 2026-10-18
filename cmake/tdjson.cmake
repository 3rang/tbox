# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
#
# tdjson.cmake - find or fetch the prebuilt TDLib JSON runtime (libtdjson).
#
# tbox loads libtdjson at run time (dlopen / LoadLibrary), so nothing here is
# linked. The only job is to have the shared library, and on Windows its
# companion DLLs, next to the tbox executable.
#
#   -DTBOX_TDJSON_DIR=<dir>   use the libtdjson you already have
#   (default)                 download the pinned tdlib.native build for the
#                             host OS/CPU into the build tree
#
# Result:  TBOX_TDJSON_DIR     directory holding libtdjson.* (+ deps)
#          TBOX_TDJSON_FILES   list of files to copy/install beside tbox

set(TBOX_TDJSON_VERSION "1.8.67" CACHE STRING "TDLib version to fetch")
set(TBOX_TDJSON_DIR "" CACHE PATH "Directory with a prebuilt libtdjson (empty = fetch)")

# Runtime identifier the tdlib.native packages use, plus the sha256 of the
# archive so a tampered or truncated download is refused.
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
  set(_pkg "tdlib.native.${_rid}.${TBOX_TDJSON_VERSION}.nupkg")
  set(_url "https://github.com/ForNeVeR/tdlib.native/releases/download/v${TBOX_TDJSON_VERSION}/${_pkg}")
  set(_dl_dir "${CMAKE_BINARY_DIR}/tdjson")
  set(_native "${_dl_dir}/runtimes/${_rid}/native")

  if(NOT IS_DIRECTORY "${_native}")
    message(STATUS "tdjson: downloading ${_url}")
    file(DOWNLOAD "${_url}" "${_dl_dir}/${_pkg}"
      EXPECTED_HASH SHA256=${_sha_${_rid}}
      SHOW_PROGRESS STATUS _st)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
      list(GET _st 1 _msg)
      message(FATAL_ERROR "tdjson: download failed: ${_msg}\n"
        "Pass -DTBOX_TDJSON_DIR=<dir containing libtdjson> to use a local copy.")
    endif()
    # A .nupkg is a zip; only the native/ folder matters.
    file(ARCHIVE_EXTRACT INPUT "${_dl_dir}/${_pkg}" DESTINATION "${_dl_dir}"
      PATTERNS "runtimes/*")
    file(REMOVE "${_dl_dir}/${_pkg}")
  endif()
  set(TBOX_TDJSON_DIR "${_native}")
endif()

file(GLOB TBOX_TDJSON_FILES
  "${TBOX_TDJSON_DIR}/*.dll" "${TBOX_TDJSON_DIR}/*.so*" "${TBOX_TDJSON_DIR}/*.dylib")
if(NOT TBOX_TDJSON_FILES)
  message(FATAL_ERROR "tdjson: no libtdjson found in ${TBOX_TDJSON_DIR}")
endif()
message(STATUS "tdjson: ${TBOX_TDJSON_DIR}")
