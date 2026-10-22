/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * scan.h — the full scan of spec 8.2: page through Saved Messages, write each
 * page to `messages`, then delete what was not seen, resolve, and record that
 * the catalog is complete.
 *
 * The scan is driven by a caller-supplied history callback rather than TDLib
 * directly, so a test can hand it the in-memory fake chat and assert on real
 * behavior. It emits sync.progress and, on success, sync.done and
 * catalog.changed through the engine's emit callback.
 *
 * The one rule that must never bend: an incomplete scan - any page that fails
 * to arrive - deletes nothing and leaves scan_complete alone. Items can only
 * ever go missing after a scan that reached the end of history.
 */
#ifndef TBOX_SYNC_SCAN_H
#define TBOX_SYNC_SCAN_H

#include <stddef.h>

#include "store/db.h"

/* Fetches one getChatHistory page. Returns a heap JSON string the scan frees
 * (a TDLib `messages` object), or NULL when the page could not be fetched.
 * The scan always asks with offset 0; paging is driven by from_message_id. */
typedef char *(*TBoxScanFetchFn)(void *ctx, long long from_message_id,
                                 int limit);

typedef struct TBoxScanHistory {
    TBoxScanFetchFn fetch;
    void *ctx;
} TBoxScanHistory;

/* One emitted protocol line (already NUL-terminated). Borrowed for the call. */
typedef void (*TBoxScanEmitFn)(const char *json_line, void *ctx);

typedef struct TBoxScanStats {
    long long messages_scanned; /* every message the scan saw */
    long long objects;          /* resolved objects afterwards */
    long long inbox_items;      /* resolved Inbox size afterwards */
    long long deleted;          /* rows removed because they were not seen */
} TBoxScanStats;

/* Runs a full scan. `op_id` names the operation in the events (a 16-hex id,
 * or NULL to omit it); `now_unix` is stamped into last_full_scan_at so the
 * scan never reads the wall clock itself. Returns 0 on a complete scan and -1
 * when any page failed, in which case nothing was deleted and scan_complete
 * was untouched. stats may be NULL. */
int tbox_scan_full(TBoxDb *db, const TBoxScanHistory *history,
                   const char *op_id, long long now_unix,
                   TBoxScanEmitFn emit, void *emit_ctx, TBoxScanStats *stats);

/* Spec 8.3 reconnect reconcile: page history from the newest message until a
 * page contains an id already in `messages` and at least two pages have been
 * read (or history ends), upserting each page. It deletes nothing - only a
 * full scan may do that - so messages deleted while offline stay until the
 * periodic scan or sync.rescan. Emits catalog.changed ["*"] when it wrote
 * anything. Returns 0 on success, -1 when a page failed (rows already
 * written stay; they are true data). */
int tbox_sync_reconcile(TBoxDb *db, const TBoxScanHistory *history,
                        TBoxScanEmitFn emit, void *emit_ctx,
                        long long *messages_read);

#endif /* TBOX_SYNC_SCAN_H */
