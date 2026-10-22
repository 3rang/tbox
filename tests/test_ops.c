/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_ops.c — the operation state machine (spec 8.4): the states, the moves
 * between them, the retry waits and the decision that is taken with an
 * operation that was still open when the engine stopped.
 *
 * The machine is the rulebook of the transfers: if it lets a move through
 * that the spec does not allow, an operation can end up in a state nobody
 * looks at, and the file it was about is lost. So every move the spec names
 * is checked here, and so is every move it does not.
 */
#include "xfer/ops.h"

#undef NDEBUG /* keep assert() alive in Release (/DNDEBUG) builds */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_names_round_trip(void) {
    static const char *const expected[] = {
        "QUEUED", "RUNNING", "SENT", "RETRY_WAIT",
        "DONE", "FAILED", "CANCELLED", "NEEDS_ATTENTION"
    };
    for (int i = 0; i < TBOX_OP_STATE_COUNT; i++) {
        assert(strcmp(tbox_op_state_name((TBoxOpState)i), expected[i]) == 0);
        TBoxOpState back = TBOX_OP_QUEUED;
        assert(tbox_op_state_parse(expected[i], &back) == 1);
        assert((int)back == i);
    }
    TBoxOpState nowhere;
    assert(tbox_op_state_parse("SOMETHING_ELSE", &nowhere) == 0);
    assert(tbox_op_state_parse("", &nowhere) == 0);
    assert(tbox_op_state_parse(NULL, &nowhere) == 0);
}

static void test_final_states(void) {
    assert(tbox_op_state_is_final(TBOX_OP_DONE) == 1);
    assert(tbox_op_state_is_final(TBOX_OP_FAILED) == 1);
    assert(tbox_op_state_is_final(TBOX_OP_CANCELLED) == 1);
    assert(tbox_op_state_is_final(TBOX_OP_NEEDS_ATTENTION) == 1);
    assert(tbox_op_state_is_final(TBOX_OP_QUEUED) == 0);
    assert(tbox_op_state_is_final(TBOX_OP_RUNNING) == 0);
    assert(tbox_op_state_is_final(TBOX_OP_SENT) == 0);
    assert(tbox_op_state_is_final(TBOX_OP_RETRY_WAIT) == 0);
}

static void test_moves(void) {
    /* the way an operation goes */
    assert(tbox_op_can_move(TBOX_OP_QUEUED, TBOX_OP_RUNNING) == 1);
    assert(tbox_op_can_move(TBOX_OP_RUNNING, TBOX_OP_SENT) == 1);
    assert(tbox_op_can_move(TBOX_OP_SENT, TBOX_OP_DONE) == 1);

    /* the way an operation waits and gives up */
    assert(tbox_op_can_move(TBOX_OP_RUNNING, TBOX_OP_RETRY_WAIT) == 1);
    assert(tbox_op_can_move(TBOX_OP_RETRY_WAIT, TBOX_OP_RUNNING) == 1);
    assert(tbox_op_can_move(TBOX_OP_RETRY_WAIT, TBOX_OP_FAILED) == 1);
    assert(tbox_op_can_move(TBOX_OP_RUNNING, TBOX_OP_FAILED) == 1);

    /* the way it is stopped, and the way it needs the user */
    assert(tbox_op_can_move(TBOX_OP_QUEUED, TBOX_OP_CANCELLED) == 1);
    assert(tbox_op_can_move(TBOX_OP_RUNNING, TBOX_OP_CANCELLED) == 1);
    assert(tbox_op_can_move(TBOX_OP_SENT, TBOX_OP_CANCELLED) == 1);
    assert(tbox_op_can_move(TBOX_OP_SENT, TBOX_OP_NEEDS_ATTENTION) == 1);
    assert(tbox_op_can_move(TBOX_OP_RUNNING, TBOX_OP_NEEDS_ATTENTION) == 1);

    /* the way the recovery puts an operation back (spec 8.4) */
    assert(tbox_op_can_move(TBOX_OP_RUNNING, TBOX_OP_QUEUED) == 1);
    assert(tbox_op_can_move(TBOX_OP_SENT, TBOX_OP_QUEUED) == 1);
    assert(tbox_op_can_move(TBOX_OP_RETRY_WAIT, TBOX_OP_QUEUED) == 1);
    /* and the way the recovery finishes one that did go out */
    assert(tbox_op_can_move(TBOX_OP_RUNNING, TBOX_OP_DONE) == 1);

    /* the moves that do not exist */
    assert(tbox_op_can_move(TBOX_OP_QUEUED, TBOX_OP_SENT) == 0);
    assert(tbox_op_can_move(TBOX_OP_QUEUED, TBOX_OP_DONE) == 0);
    assert(tbox_op_can_move(TBOX_OP_SENT, TBOX_OP_RUNNING) == 0);
    assert(tbox_op_can_move(TBOX_OP_RETRY_WAIT, TBOX_OP_DONE) == 0);

    /* a final state has no way out */
    assert(tbox_op_can_move(TBOX_OP_DONE, TBOX_OP_RUNNING) == 0);
    assert(tbox_op_can_move(TBOX_OP_DONE, TBOX_OP_QUEUED) == 0);
    assert(tbox_op_can_move(TBOX_OP_FAILED, TBOX_OP_QUEUED) == 0);
    assert(tbox_op_can_move(TBOX_OP_CANCELLED, TBOX_OP_QUEUED) == 0);
    assert(tbox_op_can_move(TBOX_OP_NEEDS_ATTENTION, TBOX_OP_QUEUED) == 0);
}

static void test_backoff(void) {
    /* the waits of spec 8.4: 5 s, 30 s, 2 min, 10 min */
    assert(tbox_op_backoff_seconds(1) == 5);
    assert(tbox_op_backoff_seconds(2) == 30);
    assert(tbox_op_backoff_seconds(3) == 120);
    assert(tbox_op_backoff_seconds(4) == 600);
    /* after that the operation has to fail */
    assert(tbox_op_backoff_seconds(5) == -1);
    assert(tbox_op_backoff_seconds(6) == -1);
    assert(tbox_op_backoff_seconds(0) == -1);
}

static void test_recovery(void) {
    /* an operation that never started is simply run again */
    assert(tbox_op_recovery(TBOX_OP_QUEUED, TBOX_OP_KIND_UPLOAD) ==
           TBOX_OP_RECOVERY_SCHEDULE);
    assert(tbox_op_recovery(TBOX_OP_RETRY_WAIT, TBOX_OP_KIND_UPLOAD) ==
           TBOX_OP_RECOVERY_SCHEDULE);
    assert(tbox_op_recovery(TBOX_OP_QUEUED, TBOX_OP_KIND_DOWNLOAD) ==
           TBOX_OP_RECOVERY_SCHEDULE);

    /* an upload that was on its way may already be in Saved Messages, so it
     * has to be looked for before it is sent again */
    assert(tbox_op_recovery(TBOX_OP_RUNNING, TBOX_OP_KIND_UPLOAD) ==
           TBOX_OP_RECOVERY_CHECK_UPLOAD);
    assert(tbox_op_recovery(TBOX_OP_SENT, TBOX_OP_KIND_UPLOAD) ==
           TBOX_OP_RECOVERY_CHECK_UPLOAD);

    /* a download that was running starts over */
    assert(tbox_op_recovery(TBOX_OP_RUNNING, TBOX_OP_KIND_DOWNLOAD) ==
           TBOX_OP_RECOVERY_RESTART_DOWNLOAD);
    assert(tbox_op_recovery(TBOX_OP_SENT, TBOX_OP_KIND_DOWNLOAD) ==
           TBOX_OP_RECOVERY_RESTART_DOWNLOAD);

    /* an operation that is over is left alone */
    assert(tbox_op_recovery(TBOX_OP_DONE, TBOX_OP_KIND_UPLOAD) ==
           TBOX_OP_RECOVERY_NONE);
    assert(tbox_op_recovery(TBOX_OP_FAILED, TBOX_OP_KIND_UPLOAD) ==
           TBOX_OP_RECOVERY_NONE);
    assert(tbox_op_recovery(TBOX_OP_CANCELLED, TBOX_OP_KIND_DOWNLOAD) ==
           TBOX_OP_RECOVERY_NONE);
    assert(tbox_op_recovery(TBOX_OP_NEEDS_ATTENTION, TBOX_OP_KIND_DOWNLOAD) ==
           TBOX_OP_RECOVERY_NONE);

    /* a kind this build does not know is not run */
    assert(tbox_op_recovery(TBOX_OP_RUNNING, "something") ==
           TBOX_OP_RECOVERY_NONE);
    assert(tbox_op_recovery(TBOX_OP_RUNNING, NULL) ==
           TBOX_OP_RECOVERY_NONE);
}

int main(void) {
    test_names_round_trip();
    test_final_states();
    test_moves();
    test_backoff();
    test_recovery();
    printf("ops OK\n");
    return 0;
}
