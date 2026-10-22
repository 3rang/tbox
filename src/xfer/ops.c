/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * ops.c — the state machine of an operation (spec 8.4). Pure: no TDLib, no
 * filesystem, no SQLite.
 *
 * The rules are written out rather than computed, because the whole point of
 * the state machine is that nobody has to guess what a move means: the
 * allowed moves are listed, and a move that is not on the list is refused.
 */
#include "xfer/ops.h"

#include <string.h>

static const char *const STATE_NAMES[TBOX_OP_STATE_COUNT] = {
    "QUEUED", "RUNNING", "SENT", "RETRY_WAIT",
    "DONE", "FAILED", "CANCELLED", "NEEDS_ATTENTION"
};

const char *tbox_op_state_name(TBoxOpState state) {
    if ((int)state < 0 || (int)state >= TBOX_OP_STATE_COUNT) {
        return "QUEUED";
    }
    return STATE_NAMES[state];
}

int tbox_op_state_parse(const char *name, TBoxOpState *out) {
    if (name == NULL) {
        return 0;
    }
    for (int i = 0; i < TBOX_OP_STATE_COUNT; i++) {
        if (strcmp(name, STATE_NAMES[i]) == 0) {
            if (out != NULL) {
                *out = (TBoxOpState)i;
            }
            return 1;
        }
    }
    return 0;
}

int tbox_op_state_is_final(TBoxOpState state) {
    switch (state) {
    case TBOX_OP_DONE:
    case TBOX_OP_FAILED:
    case TBOX_OP_CANCELLED:
    case TBOX_OP_NEEDS_ATTENTION:
        return 1;
    default:
        return 0;
    }
}

int tbox_op_can_move(TBoxOpState from, TBoxOpState to) {
    switch (from) {
    case TBOX_OP_QUEUED:
        /* it starts, or it never will */
        return to == TBOX_OP_RUNNING || to == TBOX_OP_FAILED ||
               to == TBOX_OP_CANCELLED;
    case TBOX_OP_RUNNING:
        /* it is on its way, or it has to wait, or it is over. It may also
         * go back to the queue: that is what the recovery does when the
         * operation never made it out (spec 8.4). And it may go straight to
         * DONE: that is what the recovery does when the operation turns out
         * to have gone out after all. */
        return to == TBOX_OP_SENT || to == TBOX_OP_RETRY_WAIT ||
               to == TBOX_OP_QUEUED || to == TBOX_OP_DONE ||
               to == TBOX_OP_FAILED || to == TBOX_OP_CANCELLED ||
               to == TBOX_OP_NEEDS_ATTENTION;
    case TBOX_OP_SENT:
        /* Telegram has it; the catalog has to catch up. If it turns out the
         * message is not there after all, the operation goes back into the
         * queue (spec 8.4). */
        return to == TBOX_OP_DONE || to == TBOX_OP_QUEUED ||
               to == TBOX_OP_NEEDS_ATTENTION || to == TBOX_OP_FAILED ||
               to == TBOX_OP_CANCELLED;
    case TBOX_OP_RETRY_WAIT:
        /* it tries again, or it gives up */
        return to == TBOX_OP_RUNNING || to == TBOX_OP_QUEUED ||
               to == TBOX_OP_FAILED || to == TBOX_OP_CANCELLED;
    default:
        /* the final states have no way out */
        return 0;
    }
}

int tbox_op_backoff_seconds(int attempt) {
    /* The waits of spec 8.4: 5 s, 30 s, 2 min, 10 min, then give up. */
    switch (attempt) {
    case 1:
        return 5;
    case 2:
        return 30;
    case 3:
        return 120;
    case 4:
        return 600;
    default:
        return -1;
    }
}

TBoxOpRecovery tbox_op_recovery(TBoxOpState state, const char *kind) {
    if (tbox_op_state_is_final(state)) {
        return TBOX_OP_RECOVERY_NONE;
    }
    if (state == TBOX_OP_QUEUED || state == TBOX_OP_RETRY_WAIT) {
        /* nothing was in flight: run it again */
        return TBOX_OP_RECOVERY_SCHEDULE;
    }
    if (state != TBOX_OP_RUNNING && state != TBOX_OP_SENT) {
        return TBOX_OP_RECOVERY_NONE;
    }
    if (kind != NULL && strcmp(kind, TBOX_OP_KIND_UPLOAD) == 0) {
        /* the message may already be in Saved Messages (the record carries
         * the op id, spec 7.2), so it has to be looked for before it is
         * sent a second time */
        return TBOX_OP_RECOVERY_CHECK_UPLOAD;
    }
    if (kind != NULL && strcmp(kind, TBOX_OP_KIND_DOWNLOAD) == 0) {
        /* a download that was running starts over */
        return TBOX_OP_RECOVERY_RESTART_DOWNLOAD;
    }
    /* a kind we do not know: do not run it, but leave the row where it is
     * so it stays visible */
    return TBOX_OP_RECOVERY_NONE;
}
