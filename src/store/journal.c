/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * journal.c — the `ops` table (see journal.h): the durable side of the
 * operation state machine (spec 8.4). Every statement is prepared per call
 * and every value is bound, so nothing that came from Telegram or the UI can
 * reach the SQL text.
 */
#include "store/journal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite3.h"
#include "xfer/ops.h"

/* The columns of a row, in the order the reads below expect them. */
#define OP_COLUMNS                                                        \
    "op_id,kind,state,args_json,temp_msg_id,result_json,error_code,"      \
    "attempts,next_try_at,created_at,updated_at"

/* ---- small helpers ---------------------------------------------------- */

/* Binds text that is always present (empty string when NULL). */
static void bind_text(sqlite3_stmt *st, int idx, const char *s) {
    sqlite3_bind_text(st, idx, s != NULL ? s : "", -1, SQLITE_TRANSIENT);
}

/* Copies a text column into a fixed buffer. */
static void copy_text(sqlite3_stmt *st, int idx, char *dst, size_t cap) {
    const unsigned char *v = sqlite3_column_text(st, idx);
    if (v == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, cap, "%s", (const char *)v);
}

/* Copies a text column into a fresh heap string (NULL when the column is
 * empty). The caller frees it. */
static char *dup_text(sqlite3_stmt *st, int idx) {
    const unsigned char *v = sqlite3_column_text(st, idx);
    if (v == NULL || v[0] == '\0') {
        return NULL;
    }
    size_t n = strlen((const char *)v);
    char *copy = (char *)malloc(n + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, v, n + 1);
    return copy;
}

/* Fills a row from the statement's current row. */
static void read_row(sqlite3_stmt *st, TBoxOpRow *row) {
    memset(row, 0, sizeof *row);
    copy_text(st, 0, row->op_id, sizeof row->op_id);
    copy_text(st, 1, row->kind, sizeof row->kind);
    copy_text(st, 2, row->state, sizeof row->state);
    row->args_json = dup_text(st, 3);
    copy_text(st, 4, row->temp_msg_id, sizeof row->temp_msg_id);
    row->result_json = dup_text(st, 5);
    row->error_code = dup_text(st, 6);
    row->attempts = sqlite3_column_int(st, 7);
    row->next_try_at = sqlite3_column_int64(st, 8);
    row->created_at = sqlite3_column_int64(st, 9);
    row->updated_at = sqlite3_column_int64(st, 10);
}

void tbox_journal_free_row(TBoxOpRow *row) {
    if (row == NULL) {
        return;
    }
    free(row->args_json);
    free(row->result_json);
    free(row->error_code);
    row->args_json = NULL;
    row->result_json = NULL;
    row->error_code = NULL;
}

void tbox_journal_free_rows(TBoxOpRow *rows, size_t count) {
    if (rows == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        tbox_journal_free_row(&rows[i]);
    }
    free(rows);
}

/* ---- writing ---------------------------------------------------------- */

TBoxDbStatus tbox_journal_insert(TBoxDb *db, const char *op_id,
                                  const char *kind, const char *args_json,
                                  long long now) {
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL || op_id == NULL || op_id[0] == '\0' || kind == NULL ||
        kind[0] == '\0') {
        return TBOX_DB_E_ARGS;
    }
    static const char *const SQL =
        "INSERT INTO ops(op_id,kind,state,args_json,attempts,created_at,"
        "updated_at) VALUES(?1,?2,?3,?4,0,?5,?5)";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h, SQL, -1, &st, NULL) != SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    bind_text(st, 1, op_id);
    bind_text(st, 2, kind);
    bind_text(st, 3, tbox_op_state_name(TBOX_OP_QUEUED));
    bind_text(st, 4, args_json);
    sqlite3_bind_int64(st, 5, now);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc == SQLITE_DONE) {
        return TBOX_DB_OK;
    }
    /* the op id is the primary key: a second one is a caller mistake, not
     * an I/O problem */
    if (rc == SQLITE_CONSTRAINT) {
        return TBOX_DB_E_ARGS;
    }
    return TBOX_DB_E_IO;
}

TBoxDbStatus tbox_journal_set_state(TBoxDb *db, const char *op_id,
                                     const char *state, const char *error_code,
                                     long long next_try_at, long long now) {
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL || op_id == NULL || op_id[0] == '\0' || state == NULL) {
        return TBOX_DB_E_ARGS;
    }
    TBoxOpState to = TBOX_OP_QUEUED;
    if (!tbox_op_state_parse(state, &to)) {
        return TBOX_DB_E_ARGS;
    }
    {
        /* The machine decides which moves exist; the journal refuses the
         * ones it does not know, so a wrong move never reaches the file. */
        TBoxOpState from = TBOX_OP_QUEUED;
        sqlite3_stmt *cur = NULL;
        if (sqlite3_prepare_v2(h, "SELECT state FROM ops WHERE op_id=?1", -1,
                               &cur, NULL) != SQLITE_OK) {
            return TBOX_DB_E_IO;
        }
        bind_text(cur, 1, op_id);
        int have = 0;
        if (sqlite3_step(cur) == SQLITE_ROW) {
            char name[24];
            copy_text(cur, 0, name, sizeof name);
            if (tbox_op_state_parse(name, &from)) {
                have = 1;
            }
        }
        sqlite3_finalize(cur);
        if (!have) {
            return TBOX_DB_E_ARGS; /* no such operation */
        }
        if (!tbox_op_can_move(from, to)) {
            return TBOX_DB_E_ARGS;
        }
    }

    static const char *const SQL =
        "UPDATE ops SET state=?2, error_code=?3, next_try_at=?4, updated_at=?5"
        " WHERE op_id=?1";
    sqlite3_stmt *up = NULL;
    if (sqlite3_prepare_v2(h, SQL, -1, &up, NULL) != SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    bind_text(up, 1, op_id);
    bind_text(up, 2, state);
    bind_text(up, 3, error_code);
    sqlite3_bind_int64(up, 4, next_try_at);
    sqlite3_bind_int64(up, 5, now);
    int rc = sqlite3_step(up);
    sqlite3_finalize(up);
    return rc == SQLITE_DONE ? TBOX_DB_OK : TBOX_DB_E_IO;
}

TBoxDbStatus tbox_journal_bump_attempts(TBoxDb *db, const char *op_id,
                                         long long now, int *attempts) {
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL || op_id == NULL || op_id[0] == '\0') {
        return TBOX_DB_E_ARGS;
    }
    static const char *const SQL =
        "UPDATE ops SET attempts=attempts+1, updated_at=?2 WHERE op_id=?1";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h, SQL, -1, &st, NULL) != SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    bind_text(st, 1, op_id);
    sqlite3_bind_int64(st, 2, now);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        return TBOX_DB_E_IO;
    }
    if (attempts != NULL) {
        sqlite3_stmt *q = NULL;
        if (sqlite3_prepare_v2(h, "SELECT attempts FROM ops WHERE op_id=?1", -1,
                               &q, NULL) != SQLITE_OK) {
            return TBOX_DB_E_IO;
        }
        bind_text(q, 1, op_id);
        *attempts = 0;
        if (sqlite3_step(q) == SQLITE_ROW) {
            *attempts = sqlite3_column_int(q, 0);
        }
        sqlite3_finalize(q);
    }
    return TBOX_DB_OK;
}

TBoxDbStatus tbox_journal_set_result(TBoxDb *db, const char *op_id,
                                      const char *result_json, long long now) {
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL || op_id == NULL || op_id[0] == '\0') {
        return TBOX_DB_E_ARGS;
    }
    static const char *const SQL =
        "UPDATE ops SET result_json=?2, updated_at=?3 WHERE op_id=?1";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h, SQL, -1, &st, NULL) != SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    bind_text(st, 1, op_id);
    bind_text(st, 2, result_json);
    sqlite3_bind_int64(st, 3, now);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? TBOX_DB_OK : TBOX_DB_E_IO;
}

TBoxDbStatus tbox_journal_set_temp_msg(TBoxDb *db, const char *op_id,
                                        const char *temp_msg_id, long long now) {
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL || op_id == NULL || op_id[0] == '\0') {
        return TBOX_DB_E_ARGS;
    }
    static const char *const SQL =
        "UPDATE ops SET temp_msg_id=?2, updated_at=?3 WHERE op_id=?1";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h, SQL, -1, &st, NULL) != SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    bind_text(st, 1, op_id);
    bind_text(st, 2, temp_msg_id);
    sqlite3_bind_int64(st, 3, now);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? TBOX_DB_OK : TBOX_DB_E_IO;
}

/* ---- reading ---------------------------------------------------------- */

TBoxDbStatus tbox_journal_get(TBoxDb *db, const char *op_id, TBoxOpRow *row,
                               int *found) {
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL || op_id == NULL || op_id[0] == '\0') {
        return TBOX_DB_E_ARGS;
    }
    if (found != NULL) {
        *found = 0;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h, "SELECT " OP_COLUMNS " FROM ops WHERE op_id=?1",
                           -1, &st, NULL) != SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    bind_text(st, 1, op_id);
    TBoxDbStatus status = TBOX_DB_OK;
    if (sqlite3_step(st) == SQLITE_ROW) {
        if (row != NULL) {
            read_row(st, row);
        }
        if (found != NULL) {
            *found = 1;
        }
    }
    sqlite3_finalize(st);
    return status;
}

/* Reads rows into a growing array. */
static TBoxDbStatus list_rows(sqlite3 *h, const char *sql, TBoxOpRow **rows,
                               size_t *count) {
    *rows = NULL;
    *count = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) != SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    TBoxDbStatus status = TBOX_DB_OK;
    size_t used = 0;
    size_t cap = 0;
    for (;;) {
        int rc = sqlite3_step(st);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            status = TBOX_DB_E_IO;
            break;
        }
        if (used == cap) {
            size_t want = cap == 0 ? 8 : cap * 2;
            TBoxOpRow *grown =
                (TBoxOpRow *)realloc(*rows, want * sizeof(TBoxOpRow));
            if (grown == NULL) {
                status = TBOX_DB_E_MEM;
                break;
            }
            *rows = grown;
            cap = want;
        }
        read_row(st, &(*rows)[used]);
        used++;
    }
    sqlite3_finalize(st);
    if (status != TBOX_DB_OK) {
        tbox_journal_free_rows(*rows, used);
        *rows = NULL;
        *count = 0;
        return status;
    }
    *count = used;
    return TBOX_DB_OK;
}

TBoxDbStatus tbox_journal_list_active(TBoxDb *db, TBoxOpRow **rows,
                                       size_t *count) {
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL || rows == NULL || count == NULL) {
        return TBOX_DB_E_ARGS;
    }
    /* The states that are not final are written out here rather than worked
     * out in SQL, so the one list of them stays in xfer/ops.h. */
    static const char *const SQL =
        "SELECT " OP_COLUMNS " FROM ops WHERE state NOT IN "
        "('DONE','FAILED','CANCELLED','NEEDS_ATTENTION') ORDER BY created_at,"
        "op_id";
    return list_rows(h, SQL, rows, count);
}

TBoxDbStatus tbox_journal_list_all(TBoxDb *db, TBoxOpRow **rows,
                                    size_t *count) {
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL || rows == NULL || count == NULL) {
        return TBOX_DB_E_ARGS;
    }
    static const char *const SQL =
        "SELECT " OP_COLUMNS " FROM ops ORDER BY created_at DESC, op_id DESC";
    return list_rows(h, SQL, rows, count);
}

TBoxDbStatus tbox_journal_count_active(TBoxDb *db, int *count) {
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL || count == NULL) {
        return TBOX_DB_E_ARGS;
    }
    static const char *const SQL =
        "SELECT count(*) FROM ops WHERE state NOT IN "
        "('DONE','FAILED','CANCELLED','NEEDS_ATTENTION')";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h, SQL, -1, &st, NULL) != SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    *count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        *count = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return TBOX_DB_OK;
}

/* ---- the recovery (spec 8.4) ------------------------------------------ */

TBoxDbStatus tbox_journal_recover(TBoxDb *db, TBoxOpSentFn sent_fn, void *ctx,
                                   long long now, TBoxOpRecoveryReport *report) {
    TBoxOpRecoveryReport local;
    memset(&local, 0, sizeof local);
    if (report == NULL) {
        report = &local;
    }
    TBoxOpRow *rows = NULL;
    size_t count = 0;
    TBoxDbStatus st = tbox_journal_list_active(db, &rows, &count);
    if (st != TBOX_DB_OK) {
        return st;
    }
    for (size_t i = 0; i < count; i++) {
        TBoxOpState state = TBOX_OP_QUEUED;
        if (!tbox_op_state_parse(rows[i].state, &state)) {
            /* a state this build does not know: leave the row alone, it
             * stays visible in transfer.list */
            continue;
        }
        report->checked++;
        switch (tbox_op_recovery(state, rows[i].kind)) {
        case TBOX_OP_RECOVERY_CHECK_UPLOAD: {
            int sent = (sent_fn != NULL) ? sent_fn(ctx, rows[i].op_id) : 0;
            const char *target =
                sent ? tbox_op_state_name(TBOX_OP_DONE)
                     : tbox_op_state_name(TBOX_OP_QUEUED);
            if (tbox_journal_set_state(db, rows[i].op_id, target, NULL, 0,
                                       now) == TBOX_DB_OK) {
                if (sent) {
                    report->completed++;
                } else {
                    report->requeued++;
                }
            }
            break;
        }
        case TBOX_OP_RECOVERY_RESTART_DOWNLOAD:
            if (tbox_journal_set_state(db, rows[i].op_id,
                                       tbox_op_state_name(TBOX_OP_QUEUED),
                                       NULL, 0, now) == TBOX_DB_OK) {
                report->requeued++;
            }
            break;
        case TBOX_OP_RECOVERY_SCHEDULE:
        case TBOX_OP_RECOVERY_NONE:
        default:
            /* a queued operation is picked up by the runner, an operation
             * whose kind we do not know is left alone */
            break;
        }
    }
    tbox_journal_free_rows(rows, count);
    return TBOX_DB_OK;
}
