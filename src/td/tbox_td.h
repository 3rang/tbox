/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox_td.h — TDLib JSON API: link-time declarations, import-library style.
 *
 * tdjson's C symbols are declared raw (msvcgt.def style) and linked via the
 * import library CMake builds from tdjson.dll (cmake/tdjson-vendored.cmake).
 * The DLLs are staged next to every binary by CMake POST_BUILD.
 *
 * Memory rules:
 *   - td_execute/receive results are valid ONLY until the next call —
 *     copy anything you keep (tbox_td_execute already returns a caller-owned copy)
 *   - the CALLER frees every string this API returns
 */
#ifndef TBOX_TD_H
#define TBOX_TD_H

#include <stddef.h>

/* --- the td_json_client C API, declared and resolved at link time --- */
#ifdef __cplusplus
extern "C" {
#endif

int   td_create_client_id(void);
void  td_send(int client_id, const char *request);
const char *td_receive(double timeout);
char *td_execute(const char *request);
void  td_set_log_verbosity_level(int level);

#ifdef __cplusplus
}
#endif

/* --- thin tbox wrappers ------------------------------------------------- */

void tbox_td_init(int verbosity);
void tbox_td_close(void);

/* Synchronous td_execute — works only for methods TDLib allows
 * synchronously (log management etc.). Returns a freshly malloc'd JSON
 * response the caller must free, or NULL if TDLib gave no reply. */
char *tbox_td_execute(const char *request_json);

#endif /* TBOX_TD_H */
