# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Tarang Patel
#
# tdjson-vendored.cmake — link against the prebuilt tdjson runtime.
#
# The vendored nupkg ships no .lib, so on MSVC we generate an import library
# from a .def listing the td_json_client C API symbols, then stage the DLLs
# next to every binary that links tdjson. Pinned version must stay in sync
# with scripts/fetch-tdjson.* — bump both together.

set(TBOX_TDJSON_VERSION "1.8.67" CACHE STRING "TDLib version pinned by scripts/fetch-tdjson.*")

set(TBOX_TDJSON_LIBRARY "" CACHE FILEPATH "Path to prebuilt libtdjson (empty = auto-detect in vendor/)")

# Targets that link tdjson and need the DLL staged beside them.
if(NOT DEFINED TBOX_TDJSON_TARGETS)
  set(TBOX_TDJSON_TARGETS tbox-engine)
endif()
list(REMOVE_DUPLICATES TBOX_TDJSON_TARGETS)

if(NOT TBOX_TDJSON_LIBRARY)
  if(WIN32)
    set(_tdjson_candidate "${CMAKE_SOURCE_DIR}/vendor/tdjson.dll")
  elseif(APPLE)
    set(_tdjson_candidate "${CMAKE_SOURCE_DIR}/vendor/libtdjson.dylib")
  else()
    set(_tdjson_candidate "${CMAKE_SOURCE_DIR}/vendor/libtdjson.so")
  endif()
  if(EXISTS "${_tdjson_candidate}")
    set(TBOX_TDJSON_LIBRARY "${_tdjson_candidate}")
  endif()
endif()

if(TBOX_TDJSON_LIBRARY)
  message(STATUS "tdjson: ${TBOX_TDJSON_LIBRARY}")

  # ---- link the td_json_client C API ------------------------------------
  if(MSVC)
    set(_def "${CMAKE_BINARY_DIR}/tdjson.def")
    set(_lib "${CMAKE_BINARY_DIR}/tdjson.lib")
    if(NOT EXISTS "${_lib}")
      set(_exports
        td_create_client_id
        td_send
        td_receive
        td_execute
        td_json_client_create
        td_json_client_destroy
        td_json_client_send
        td_json_client_receive
        td_json_client_execute
        td_set_log_verbosity_level
        td_set_log_file_path
        td_set_log_max_file_size
        td_set_log_message_callback
        td_set_log_fatal_error_callback
      )
      file(WRITE "${_def}" "LIBRARY tdjson\nEXPORTS\n")
      foreach(_sym ${_exports})
        file(APPEND "${_def}" "    ${_sym}\n")
      endforeach()
      execute_process(
        COMMAND lib /nologo /machine:x64 /def:${_def} /out:${_lib}
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
    endif()
    if(NOT EXISTS "${_lib}")
      message(FATAL_ERROR
        "Failed to build tdjson.lib (run inside a vcvars/Developer prompt so 'lib' is on PATH)")
    endif()
    foreach(_tgt ${TBOX_TDJSON_TARGETS})
      target_link_libraries(${_tgt} PRIVATE "${_lib}")
    endforeach()
  else()
    foreach(_tgt ${TBOX_TDJSON_TARGETS})
      target_link_libraries(${_tgt} PRIVATE "${TBOX_TDJSON_LIBRARY}")
    endforeach()
  endif()

  # ---- stage runtime DLLs next to each binary ---------------------------
  set(_tdjson_runtime_files "${TBOX_TDJSON_LIBRARY}")
  if(WIN32)
    file(GLOB _vendor_dlls "${CMAKE_SOURCE_DIR}/vendor/*.dll")
    list(APPEND _tdjson_runtime_files ${_vendor_dlls})
  endif()
  foreach(_tgt ${TBOX_TDJSON_TARGETS})
    add_custom_command(TARGET ${_tgt} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              ${_tdjson_runtime_files} "$<TARGET_FILE_DIR:${_tgt}>")
  endforeach()
else()
  message(FATAL_ERROR
    "libtdjson not found in vendor/. Run scripts/fetch-tdjson.ps1 (Windows) "
    "or scripts/fetch-tdjson.sh (Linux/macOS) first.")
endif()
