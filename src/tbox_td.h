/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox_td.h — native TDLib bridge: load tdjson, synchronous execute.
 *
 * Purity rules for the whole C core:
 *   - no environment variables, no known paths, no global state
 *   - callers pass in every path (the library path here)
 *   - memory rule: the CALLER frees every string this API returns
 */
#ifndef TBOX_TD_H
#define TBOX_TD_H

typedef struct TBoxTd TBoxTd;

/* Load the tdjson shared library from an explicit path.
 * Returns NULL (nothing is loaded) if the path is bad or loading fails. */
TBoxTd *tbox_td_open(const char *library_path);

/* Synchronous td_execute — works only for the few methods TDLib allows
 * synchronously (log management etc.). Returns a freshly malloc'd JSON
 * response the caller must free, or NULL if TDLib gave no reply. */
char *tbox_td_execute(TBoxTd *td, const char *request_json);

/* Verbosity 0..5 (0 = quiet). Safe no-op before the library loaded symbols. */
void tbox_td_set_verbosity(TBoxTd *td, int level);

/* Unload the library. NULL is allowed (no-op). */
void tbox_td_close(TBoxTd *td);

#endif /* TBOX_TD_H */
