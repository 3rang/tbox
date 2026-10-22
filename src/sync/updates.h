/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * updates.h — incremental updates from Saved Messages (spec 8.3).
 *
 * Where scan.c rebuilds the catalog from all of history, this module applies
 * the single-message updates TDLib sends while the app is running: a new
 * message, an edit, a real delete. For each one it writes just that row, then
 * re-derives `objects` from the messages table, so the incremental path and a
 * fresh full scan can never disagree (that equivalence is the whole point of
 * task 1.6 and its test).
 *
 * Updates for any other chat are ignored, as are the ones only Phase 2 cares
 * about (updateFile, updateMessageSendFailed, op mapping beyond the temp id).
 * A `from_cache` delete is ignored too: it is TDLib evicting a local copy, not
 * the user deleting a message.
 *
 * catalog.changed is emitted as ["*"] - the spec's "refresh everything" form.
 * Rebuilding from the messages table already touches every folder, so naming
 * a narrower set would only risk naming the wrong one.
 */
#ifndef TBOX_SYNC_UPDATES_H
#define TBOX_SYNC_UPDATES_H

#include <stddef.h>

#include "store/db.h"

/* Fetches one message by id (a getMessage reply) as heap JSON, or NULL when it
 * could not be read. Used by the edit updates, which carry only an id. */
typedef char *(*TBoxUpdateFetchFn)(void *ctx, long long message_id);

/* One emitted protocol line (already NUL-terminated). Borrowed for the call. */
typedef void (*TBoxUpdateEmitFn)(const char *json_line, void *ctx);

typedef struct TBoxUpdates TBoxUpdates;

/* What one update did. */
typedef struct TBoxUpdateOutcome {
    int handled;      /* 1 when the update was one this module acts on */
    int needs_resync; /* 1 when it could not be applied safely */
    int changed;      /* 1 when catalog.changed was emitted */
} TBoxUpdateOutcome;

/* Creates an updater for one Saved Messages chat (`chat_id`) writing to `db`.
 * fetch may be NULL if no edit updates are expected (tests that only add and
 * delete). emit may be NULL. Returns NULL on bad arguments or out of memory. */
TBoxUpdates *tbox_updates_new(TBoxDb *db, long long chat_id,
                              TBoxUpdateFetchFn fetch, void *fetch_ctx,
                              TBoxUpdateEmitFn emit, void *emit_ctx);

void tbox_updates_free(TBoxUpdates *u);

/* Applies one TDLib update object (JSON text). Returns 0 when it was parsed
 * and handled or deliberately ignored, and -1 on malformed JSON or a write
 * failure (in which case nothing is emitted). `out` may be NULL. */
int tbox_updates_apply(TBoxUpdates *u, const char *update_json,
                       TBoxUpdateOutcome *out);

/* Rebuilds `objects` from the current `messages` table and, when
 * emit_changed is non-zero, emits catalog.changed ["*"]. The engine also uses
 * this after recovery. Returns 0 on success, -1 on a write failure. */
int tbox_updates_reresolve(TBoxUpdates *u, int emit_changed);

#endif /* TBOX_SYNC_UPDATES_H */
