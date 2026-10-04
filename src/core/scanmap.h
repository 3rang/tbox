/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TBOX_CORE_SCANMAP_H
#define TBOX_CORE_SCANMAP_H

#include "core/index.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Scan mapping (core layer): turn one Saved Messages document into one
 * archive entry, exactly the way v0.3's search.py did.
 *
 * Pure by rule - it takes already-extracted JSON fields, never parses a
 * message - so the whole naming/metadata decision is covered by
 * tests/test_scanmap.c. src/td/tdscan.c does the TDLib plumbing and calls
 * this once per message that carries a document.
 *
 * Naming, in order:
 *   1. our caption's "name"           (a tbox file or dir marker)
 *   2. the document's own file_name   (a foreign document)
 *   3. "message-<id>"                 (both missing/unusable)
 *
 * Metadata fallbacks, so a listing never shows a 1970 date:
 *   mtime <- caption, else the message date
 *   size  <- caption, else the document size
 *
 * `text`, `file_name` and `date` may be NULL/0 when Telegram did not supply
 * them; `message_id` must be a real (positive) message id. The caller owns
 * nothing here: `out` is a plain struct.
 *
 * Returns 0 on success, -1 when `out` is NULL.
 */
int tbox_scanmap_entry(long long message_id, const char *text,
                       const char *file_name, long long size,
                       long long date, long long remote_id,
                       tbox_archive_entry_t *out);

#ifdef __cplusplus
}
#endif

#endif /* TBOX_CORE_SCANMAP_H */
