/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_journal.c — the ops journal (spec 8.4): what it writes, what it
 * refuses to write, and the recovery of an operation that was still open
 * when the engine stopped.
 *
 * The rule that matters most is the one about uploads: an operation whose
 * message is already in Saved Messages must not be sent a second time. The
 * record of a sent file carries the id of the operation that sent it (the
 * `q` key, spec 7.2), so the journal can tell. Every state is killed and
 * restarted here, because the moment an operation can be lost is exactly the
 * moment the engine dies in the middle of it.
 */
#include "store/journal.h"

#undef NDEBUG /* keep assert() alive in Release (/DNDEBUG) builds */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog/message.h"
#include "store/catalog.h"
#include "store/db.h"
#include "util/fsx.h"
#include "xfer/ops.h"

#define DIR "tbox_journal_test"

/* A fresh database: the journal of one run must not leak into the next. */
static TBoxDb *open_fresh(void) {
    assert(tbox_fsx_mkdirs(DIR) == TBOX_FSX_OK);
    tbox_fsx_remove(DIR "/index.db");
    tbox_fsx_remove(DIR "/index.db-wal");
    tbox_fsx_remove(DIR "/index.db-shm");
    TBoxDb *db = NULL;
    assert(tbox_db_open(DIR, &db) == TBOX_DB_OK);
    assert(db != NULL);
    return db;
}

/* The state an operation is in. */
static const char *state_of(TBoxDb *db, const char *op_id) {
    static char name[24];
    TBoxOpRow row;
    memset(&row, 0, sizeof row);
    int found = 0;
    assert(tbox_journal_get(db, op_id, &row, &found) == TBOX_DB_OK);
    assert(found == 1);
    snprintf(name, sizeof name, "%s", row.state);
    tbox_journal_free_row(&row);
    return name;
}

/* Puts a message in the catalog that carries the record of an operation:
 * that is how a sent upload looks to the engine afterwards. */
static void add_sent_message(TBoxDb *db, const char *message_id,
                              const char *op_id, const char *object_id) {
    TBoxMessageRow row;
    memset(&row, 0, sizeof row);
    snprintf(row.message_id, sizeof row.message_id, "%s", message_id);
    snprintf(row.kind, sizeof row.kind, "%s", TBOX_MSG_KIND_FILE);
    row.date = 1760000000;
    row.has_record = 1;
    snprintf(row.rec.object_id, sizeof row.rec.object_id, "%s", object_id);
    snprintf(row.rec.parent, sizeof row.rec.parent, "root");
    snprintf(row.rec.name, sizeof row.rec.name, "sent.txt");
    row.rec.rev = 1;
    row.rec.kind = 'f';
    row.rec.has_op = 1;
    snprintf(row.rec.op_id, sizeof row.rec.op_id, "%s", op_id);
    assert(tbox_cat_upsert_message(db, &row) == TBOX_DB_OK);
}

/* The engine's answer to "has this operation already gone out?". */
static int op_sent(void *ctx, const char *op_id) {
    TBoxDb *db = (TBoxDb *)ctx;
    return tbox_cat_find_op_message(db, op_id, NULL);
}

static void test_insert_and_read(void) {
    TBoxDb *db = open_fresh();

    assert(tbox_journal_insert(db, "0000000000000001", TBOX_OP_KIND_UPLOAD,
                                "{\"local_path\":\"a.txt\"}", 100) ==
           TBOX_DB_OK);

    TBoxOpRow row;
    memset(&row, 0, sizeof row);
    int found = 0;
    assert(tbox_journal_get(db, "0000000000000001", &row, &found) ==
           TBOX_DB_OK);
    assert(found == 1);
    assert(strcmp(row.op_id, "0000000000000001") == 0);
    assert(strcmp(row.kind, TBOX_OP_KIND_UPLOAD) == 0);
    /* a new operation waits in the queue */
    assert(strcmp(row.state, "QUEUED") == 0);
    assert(row.attempts == 0);
    assert(row.args_json != NULL);
    assert(strcmp(row.args_json, "{\"local_path\":\"a.txt\"}") == 0);
    assert(row.created_at == 100);
    assert(row.updated_at == 100);
    tbox_journal_free_row(&row);

    /* an operation that is not there is simply not there */
    memset(&row, 0, sizeof row);
    found = 1;
    assert(tbox_journal_get(db, "0000000000000002", &row, &found) ==
           TBOX_DB_OK);
    assert(found == 0);
    tbox_journal_free_row(&row);

    /* the same id twice is refused: the second one would overwrite the
     * state of a running operation */
    assert(tbox_journal_insert(db, "0000000000000001", TBOX_OP_KIND_UPLOAD,
                                "{}", 101) == TBOX_DB_E_ARGS);

    tbox_db_close(db);
}

static void test_state_moves(void) {
    TBoxDb *db = open_fresh();
    assert(tbox_journal_insert(db, "0000000000000003", TBOX_OP_KIND_UPLOAD,
                                "{}", 100) == TBOX_DB_OK);

    /* the way an operation goes */
    assert(tbox_journal_set_state(db, "0000000000000003", "RUNNING", NULL, 0,
                                   101) == TBOX_DB_OK);
    assert(strcmp(state_of(db, "0000000000000003"), "RUNNING") == 0);
    assert(tbox_journal_set_state(db, "0000000000000003", "SENT", NULL, 0,
                                   102) == TBOX_DB_OK);
    assert(tbox_journal_set_state(db, "0000000000000003", "DONE", NULL, 0,
                                   103) == TBOX_DB_OK);
    assert(strcmp(state_of(db, "0000000000000003"), "DONE") == 0);

    /* a move the machine does not allow never reaches the file */
    assert(tbox_journal_set_state(db, "0000000000000003", "RUNNING", NULL, 0,
                                   104) == TBOX_DB_E_ARGS);
    assert(strcmp(state_of(db, "0000000000000003"), "DONE") == 0);

    /* nor does a move to a state that does not exist */
    assert(tbox_journal_set_state(db, "0000000000000003", "MAYBE", NULL, 0,
                                   105) == TBOX_DB_E_ARGS);

    /* and there is no operation to move */
    assert(tbox_journal_set_state(db, "0000000000000009", "RUNNING", NULL, 0,
                                   106) == TBOX_DB_E_ARGS);

    /* the tries are counted, and a failure carries its code */
    assert(tbox_journal_insert(db, "0000000000000004", TBOX_OP_KIND_UPLOAD,
                                "{}", 100) == TBOX_DB_OK);
    int attempts = 0;
    assert(tbox_journal_bump_attempts(db, "0000000000000004", 110, &attempts) ==
           TBOX_DB_OK);
    assert(attempts == 1);
    assert(tbox_journal_bump_attempts(db, "0000000000000004", 111, &attempts) ==
           TBOX_DB_OK);
    assert(attempts == 2);
    assert(tbox_journal_set_state(db, "0000000000000004", "FAILED",
                                   "FLOOD_WAIT", 120, 112) == TBOX_DB_OK);
    TBoxOpRow row;
    memset(&row, 0, sizeof row);
    int found = 0;
    assert(tbox_journal_get(db, "0000000000000004", &row, &found) ==
           TBOX_DB_OK);
    assert(found == 1);
    assert(strcmp(row.state, "FAILED") == 0);
    assert(row.error_code != NULL && strcmp(row.error_code, "FLOOD_WAIT") == 0);
    assert(row.next_try_at == 120);
    tbox_journal_free_row(&row);

    /* the message an operation is waiting for, and what it produced */
    assert(tbox_journal_set_temp_msg(db, "0000000000000004", "12345", 113) ==
           TBOX_DB_OK);
    assert(tbox_journal_set_result(db, "0000000000000004",
                                   "{\"object_id\":\"abc\"}", 114) ==
           TBOX_DB_OK);
    memset(&row, 0, sizeof row);
    assert(tbox_journal_get(db, "0000000000000004", &row, &found) ==
           TBOX_DB_OK);
    assert(strcmp(row.temp_msg_id, "12345") == 0);
    assert(row.result_json != NULL);
    assert(strcmp(row.result_json, "{\"object_id\":\"abc\"}") == 0);
    tbox_journal_free_row(&row);

    tbox_db_close(db);
}

static void test_lists(void) {
    TBoxDb *db = open_fresh();
    assert(tbox_journal_insert(db, "0000000000000001", TBOX_OP_KIND_UPLOAD,
                                "{}", 100) == TBOX_DB_OK);
    assert(tbox_journal_insert(db, "0000000000000002", TBOX_OP_KIND_UPLOAD,
                                "{}", 101) == TBOX_DB_OK);
    assert(tbox_journal_insert(db, "0000000000000003", TBOX_OP_KIND_DOWNLOAD,
                                "{}", 102) == TBOX_DB_OK);

    /* one of them is finished, the other two are not */
    assert(tbox_journal_set_state(db, "0000000000000002", "RUNNING", NULL, 0,
                                   103) == TBOX_DB_OK);
    assert(tbox_journal_set_state(db, "0000000000000002", "SENT", NULL, 0,
                                   104) == TBOX_DB_OK);
    assert(tbox_journal_set_state(db, "0000000000000002", "DONE", NULL, 0,
                                   105) == TBOX_DB_OK);

    TBoxOpRow *rows = NULL;
    size_t count = 0;
    assert(tbox_journal_list_active(db, &rows, &count) == TBOX_DB_OK);
    assert(count == 2);
    tbox_journal_free_rows(rows, count);

    assert(tbox_journal_list_all(db, &rows, &count) == TBOX_DB_OK);
    assert(count == 3);
    tbox_journal_free_rows(rows, count);

    int pending = 0;
    assert(tbox_journal_count_active(db, &pending) == TBOX_DB_OK);
    assert(pending == 2);

    tbox_db_close(db);
}

/* The rule of spec 8.4: an upload that was running when the engine died may
 * already be in Saved Messages. If it is there, the operation is done and is
 * not sent again; if it is not, it goes back into the queue. */
static void test_recovery_does_not_send_an_upload_twice(void) {
    TBoxDb *db = open_fresh();

    /* two uploads: one that went out, one that did not */
    assert(tbox_journal_insert(db, "00000000000000aa", TBOX_OP_KIND_UPLOAD,
                                "{}", 100) == TBOX_DB_OK);
    assert(tbox_journal_set_state(db, "00000000000000aa", "RUNNING", NULL, 0,
                                   101) == TBOX_DB_OK);
    assert(tbox_journal_insert(db, "00000000000000bb", TBOX_OP_KIND_UPLOAD,
                                "{}", 102) == TBOX_DB_OK);
    assert(tbox_journal_set_state(db, "00000000000000bb", "RUNNING", NULL, 0,
                                   103) == TBOX_DB_OK);

    /* the first one is in Saved Messages: its record carries the op id */
    add_sent_message(db, "9001", "00000000000000aa",
                     "1111111111111111111111111111aaaa");
    {
        TBoxOpMessage found;
        memset(&found, 0, sizeof found);
        assert(tbox_cat_find_op_message(db, "00000000000000aa", &found) == 1);
        assert(strcmp(found.message_id, "9001") == 0);
        assert(strcmp(found.object_id, "1111111111111111111111111111aaaa") == 0);
    }

    TBoxOpRecoveryReport report;
    memset(&report, 0, sizeof report);
    assert(tbox_journal_recover(db, op_sent, db, 200, &report) == TBOX_DB_OK);
    assert(report.checked == 2);
    assert(report.completed == 1);
    assert(report.requeued == 1);

    /* the one that went out is done: it is never sent again */
    assert(strcmp(state_of(db, "00000000000000aa"), "DONE") == 0);
    /* the one that did not goes back into the queue */
    assert(strcmp(state_of(db, "00000000000000bb"), "QUEUED") == 0);

    /* a second recovery does not touch the finished one again */
    memset(&report, 0, sizeof report);
    assert(tbox_journal_recover(db, op_sent, db, 201, &report) == TBOX_DB_OK);
    assert(report.checked == 1); /* only the one that is still open */
    assert(report.completed == 0);
    assert(strcmp(state_of(db, "00000000000000bb"), "QUEUED") == 0);

    tbox_db_close(db);
}

/* Every state an operation can die in, and what the engine does with it. */
static void test_recovery_at_every_state(void) {
    TBoxDb *db = open_fresh();

    /* a download that was running starts over */
    assert(tbox_journal_insert(db, "00000000000000d1", TBOX_OP_KIND_DOWNLOAD,
                                "{}", 100) == TBOX_DB_OK);
    assert(tbox_journal_set_state(db, "00000000000000d1", "RUNNING", NULL, 0,
                                   101) == TBOX_DB_OK);
    /* an upload that was sent, and whose message is not there */
    assert(tbox_journal_insert(db, "00000000000000d2", TBOX_OP_KIND_UPLOAD,
                                "{}", 102) == TBOX_DB_OK);
    assert(tbox_journal_set_state(db, "00000000000000d2", "RUNNING", NULL, 0,
                                   103) == TBOX_DB_OK);
    assert(tbox_journal_set_state(db, "00000000000000d2", "SENT", NULL, 0,
                                   104) == TBOX_DB_OK);
    /* an upload that is waiting for its turn */
    assert(tbox_journal_insert(db, "00000000000000d3", TBOX_OP_KIND_UPLOAD,
                                "{}", 105) == TBOX_DB_OK);
    assert(tbox_journal_set_state(db, "00000000000000d3", "RUNNING", NULL, 0,
                                   106) == TBOX_DB_OK);
    assert(tbox_journal_set_state(db, "00000000000000d3", "RETRY_WAIT", NULL,
                                   200, 107) == TBOX_DB_OK);
    /* an upload that never started */
    assert(tbox_journal_insert(db, "00000000000000d4", TBOX_OP_KIND_UPLOAD,
                                "{}", 108) == TBOX_DB_OK);
    /* an operation that is over: it is not looked at */
    assert(tbox_journal_insert(db, "00000000000000d5", TBOX_OP_KIND_UPLOAD,
                                "{}", 109) == TBOX_DB_OK);
    assert(tbox_journal_set_state(db, "00000000000000d5", "FAILED", "NOPE", 0,
                                   110) == TBOX_DB_OK);

    TBoxOpRecoveryReport report;
    memset(&report, 0, sizeof report);
    assert(tbox_journal_recover(db, op_sent, db, 300, &report) == TBOX_DB_OK);
    assert(report.checked == 4); /* the failed one is not looked at */
    assert(report.completed == 0);
    assert(report.requeued == 2); /* the download and the sent upload */

    assert(strcmp(state_of(db, "00000000000000d1"), "QUEUED") == 0);
    assert(strcmp(state_of(db, "00000000000000d2"), "QUEUED") == 0);
    /* a waiting operation keeps its wait */
    assert(strcmp(state_of(db, "00000000000000d3"), "RETRY_WAIT") == 0);
    /* a queued operation stays queued */
    assert(strcmp(state_of(db, "00000000000000d4"), "QUEUED") == 0);
    /* a failed operation is left alone */
    assert(strcmp(state_of(db, "00000000000000d5"), "FAILED") == 0);

    tbox_db_close(db);
}

int main(void) {
    test_insert_and_read();
    test_state_moves();
    test_lists();
    test_recovery_does_not_send_an_upload_twice();
    test_recovery_at_every_state();
    printf("journal OK\n");
    return 0;
}
