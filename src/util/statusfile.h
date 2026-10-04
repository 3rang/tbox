/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TBOX_STATUSFILE_H
#define TBOX_STATUSFILE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * status.json - the seam between `tbox serve` and everything that wants to
 * know whether the archive is up (util layer: no TDLib, no network).
 *
 * Written by the owner on every state change, replaced atomically so a
 * reader (a tray icon, a script, `tbox status`) can never observe a
 * half-written file:
 *
 *   {"v":1,"state":"ready","user":"tpat","phone":"+91...","since":"...Z",
 *    "transport":"ftp","endpoint":"127.0.0.1:2121","indexed":128,
 *    "cache_bytes":1048576,"pid":4242,"error":null}
 *
 * `tbox status` reads only this file - it never opens TDLib - so it works
 * while serve runs and after it died (a dead pid means "stale").
 */

#define TBOX_STATUS_V 1

typedef enum
{
    TBOX_STATE_UNKNOWN = 0,   /* also the zero value of a fresh struct */
    TBOX_STATE_STARTING,
    TBOX_STATE_QR_REQUIRED,
    TBOX_STATE_AUTHORIZING,
    TBOX_STATE_READY,
    TBOX_STATE_STOPPED,       /* clean shutdown */
    TBOX_STATE_ERROR

} tbox_state_t;

/* The wire name ("ready"), or "unknown". */
const char *tbox_state_name(tbox_state_t state);

/* Parse a wire name; TBOX_STATE_UNKNOWN when it is not one of ours. */
tbox_state_t tbox_state_from_name(const char *name);

/*
 * Is this state one that only a *running* serve can be in? Used to tell
 * "serve is up" from "serve died and left this behind".
 */
int tbox_state_is_running(tbox_state_t state);

typedef struct
{
    tbox_state_t state;
    char user[64];             /* first name, "" when unknown */
    char phone[32];            /* "+91..." , "" when unknown */
    char since[32];            /* ISO-8601 UTC, e.g. 2026-10-04T09:00:00Z */
    char transport[16];        /* "ftp" / "sftp", "" when none is up */
    char endpoint[64];         /* "127.0.0.1:2121", "" when none */
    long long indexed;         /* entries in the archive index */
    long long cache_bytes;     /* local file cache size */
    long pid;                  /* owner process, 0 when unknown */
    char error[256];           /* "" when there is none */

} tbox_status_t;

/* "<root>/status.json" into `out`; returns 0 or -1 if it would not fit. */
int tbox_status_path(char *out, size_t size, const char *root);

/* Zero the struct and set pid to this process, state to "starting". */
void tbox_status_init(tbox_status_t *status);

/* Stamp `since` with the current UTC time (20 chars + NUL). */
void tbox_status_now(char *buf, size_t size);

/*
 * Serialize and replace `path` atomically (write "<path>.tmp", then rename
 * over it). Returns 0, or -1 on any I/O or allocation failure.
 */
int tbox_status_write(const char *path, const tbox_status_t *status);

/*
 * Read `path` into `out`. Returns 0, or -1 when the file is missing,
 * unreadable, not JSON, or not a version 1 status.
 */
int tbox_status_read(const char *path, tbox_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* TBOX_STATUSFILE_H */