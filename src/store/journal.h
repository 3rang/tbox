/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * journal.h — the `ops` table of index.db: the journal of what tbox was asked
 * to do (spec 7.5, 8.4). An operation lives here from the moment it is
 * accepted until it is done, so an engine that dies in the middle leaves a
 * trail the next run can pick up.
 *
 * The state machine is pure and lives in xfer/ops.h; this module is the
 * durable side of it. Every statement is prepared per call and every value
 * is bound, never spliced into SQL.
 */
#ifndef TBOX_STORE_JOURNAL_H
#define TBOX_STORE_JOURNAL_H

#include <stddef.h>

#include "catalog/record.h" /* TBOX_OP_HEX_LEN, TBOX_MSGID_MAX_BYTES */
#include "store/db.h"

/* One row of the `ops` table. The short fields are fixed copies; the JSON
 * payloads are heap text that tbox_journal_free_row releases. */
typedef struct TBoxOpRow {
    char      op_id[TBOX_OP_HEX_LEN + 1];
    char      kind[16];
    char      state[24];
    char     *args_json;   /* the request that started the operation */
    char     *result_json; /* what it produced, once it is done */
    char     *error_code;  /* the error code of spec 6.3, when it failed */
    char      temp_msg_id[TBOX_MSGID_MAX_BYTES + 1];
    int       attempts;
    long long next_try_at; /* Unix seconds; 0 when there is no wait */
    long long created_at;
    long long updated_at;
} TBoxOpRow;

/* Writes a new operation in the QUEUED state. The op id is the one the
 * protocol hands out; it is also what the record of a sent file carries
 * (spec 7.2), so the same operation can be recognized later. */
TBoxDbStatus tbox_journal_insert(TBoxDb *db, const char *op_id,
                                  const char *kind, const char *args_json,
                                  long long now);

/* Moves an operation to another state. error_code may be NULL, and
 * next_try_at is 0 when the operation does not wait. */
TBoxDbStatus tbox_journal_set_state(TBoxDb *db, const char *op_id,
                                     const char *state, const char *error_code,
                                     long long next_try_at, long long now);

/* Counts one more try and hands the new count back. */
TBoxDbStatus tbox_journal_bump_attempts(TBoxDb *db, const char *op_id,
                                         long long now, int *attempts);

/* Records what the operation produced. */
TBoxDbStatus tbox_journal_set_result(TBoxDb *db, const char *op_id,
                                      const char *result_json, long long now);

/* Points the operation at the message id it is waiting for (the temp id of
 * an upload, spec 8.3). */
TBoxDbStatus tbox_journal_set_temp_msg(TBoxDb *db, const char *op_id,
                                        const char *temp_msg_id, long long now);

/* Reads one operation. Returns TBOX_DB_OK and sets *found; the row is left
 * untouched when the operation is not there. */
TBoxDbStatus tbox_journal_get(TBoxDb *db, const char *op_id, TBoxOpRow *row,
                               int *found);

/* Lists the operations that are not final: the ones the engine still has to
 * deal with. Free the array with tbox_journal_free_rows(). */
TBoxDbStatus tbox_journal_list_active(TBoxDb *db, TBoxOpRow **rows,
                                       size_t *count);

/* Lists every operation, newest first. Free the array with
 * tbox_journal_free_rows(). */
TBoxDbStatus tbox_journal_list_all(TBoxDb *db, TBoxOpRow **rows, size_t *count);

/* How many operations are not final (engine.status: pending_ops). */
TBoxDbStatus tbox_journal_count_active(TBoxDb *db, int *count);

/* Frees the heap fields of a row. Safe on a zeroed row and safe twice. */
void tbox_journal_free_row(TBoxOpRow *row);
void tbox_journal_free_rows(TBoxOpRow *rows, size_t count);

/* Answers "has this operation already gone out?" - the engine looks the
 * message up in the catalog, where the record of a sent file carries the op
 * id (spec 7.2). */
typedef int (*TBoxOpSentFn)(void *ctx, const char *op_id);

/* What the recovery did. */
typedef struct TBoxOpRecoveryReport {
    int checked;   /* the operations that were looked at */
    int completed; /* found to have gone out already: not sent again */
    int requeued;  /* put back in the queue */
} TBoxOpRecoveryReport;

/* The recovery of spec 8.4, applied to the operations that were still open
 * when the engine stopped. An upload that was running or sent may already
 * be in Saved Messages, so it is looked for first (sent_fn); if it is not
 * there the operation goes back in the queue. A download that was running
 * starts over. `now` is the timestamp the rows are stamped with. */
TBoxDbStatus tbox_journal_recover(TBoxDb *db, TBoxOpSentFn sent_fn, void *ctx,
                                   long long now, TBoxOpRecoveryReport *report);

#endif /* TBOX_STORE_JOURNAL_H */
