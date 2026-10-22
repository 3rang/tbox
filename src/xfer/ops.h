/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * ops.h — the state machine of an operation (spec 8.4). Pure: no TDLib, no
 * filesystem, no SQLite, so it can be reasoned about (and tested) on its own.
 *
 * An operation is one thing tbox was asked to do and has not finished yet:
 * an upload (spec 8.5) or a download (spec 8.6). It lives in the `ops`
 * table of index.db, so it survives a crash and the engine can pick it up
 * again after a restart.
 *
 * The states and the moves between them are the ones of spec 8.4:
 *
 *     QUEUED -> RUNNING -> SENT -> DONE
 *                 |          |
 *                 v          v
 *             RETRY_WAIT  NEEDS_ATTENTION
 *                 |
 *                 v
 *               FAILED     CANCELLED
 *
 * DONE, FAILED, CANCELLED and NEEDS_ATTENTION are final: an operation in
 * one of them is never picked up again.
 */
#ifndef TBOX_XFER_OPS_H
#define TBOX_XFER_OPS_H

/* The states of an operation (spec 8.4). */
typedef enum TBoxOpState {
    TBOX_OP_QUEUED = 0,
    TBOX_OP_RUNNING,
    TBOX_OP_SENT,
    TBOX_OP_RETRY_WAIT,
    TBOX_OP_DONE,
    TBOX_OP_FAILED,
    TBOX_OP_CANCELLED,
    TBOX_OP_NEEDS_ATTENTION,
    TBOX_OP_STATE_COUNT
} TBoxOpState;

/* The kinds of operation (spec 8.5, 8.6). */
#define TBOX_OP_KIND_UPLOAD   "upload"
#define TBOX_OP_KIND_DOWNLOAD "download"

/* The name the `ops` table stores for a state. */
const char *tbox_op_state_name(TBoxOpState state);

/* The state a stored name means. Returns 1 and fills *out for a known name,
 * 0 for anything else (the caller then treats the row as unreadable). */
int tbox_op_state_parse(const char *name, TBoxOpState *out);

/* 1 for the states an operation never leaves: DONE, FAILED, CANCELLED and
 * NEEDS_ATTENTION. */
int tbox_op_state_is_final(TBoxOpState state);

/* 1 when an operation may move from one state to another (spec 8.4). A move
 * that the machine does not allow is a bug in the caller, not a state the
 * database may hold. */
int tbox_op_can_move(TBoxOpState from, TBoxOpState to);

/* How long to wait before the next try (spec 8.4): 5 s, 30 s, 2 min, 10 min.
 * The attempt number counts the failures, so the first failure waits 5 s.
 * Returns -1 when the operation has used up its tries and has to fail. */
int tbox_op_backoff_seconds(int attempt);

/* What to do with an operation that was still open when the engine stopped
 * (spec 8.4). */
typedef enum TBoxOpRecovery {
    TBOX_OP_RECOVERY_NONE = 0,       /* it is over: nothing to do */
    TBOX_OP_RECOVERY_SCHEDULE,        /* run it again */
    TBOX_OP_RECOVERY_CHECK_UPLOAD,    /* it may already have gone out */
    TBOX_OP_RECOVERY_RESTART_DOWNLOAD /* it has to start over */
} TBoxOpRecovery;

/* The recovery for an operation of this state and kind. An upload that was
 * running or sent may already be in Saved Messages (its record carries the
 * op id, spec 7.2), so it has to be looked for before it is sent again; a
 * download that was running simply starts over. */
TBoxOpRecovery tbox_op_recovery(TBoxOpState state, const char *kind);

#endif /* TBOX_XFER_OPS_H */
