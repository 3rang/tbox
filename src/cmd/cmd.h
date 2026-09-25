/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TBOX_CMD_H
#define TBOX_CMD_H

/* ---- exit codes (ladder decision #4 + stub code) ------------------------- */
#define TBOX_EXIT_OK        0   /* success */
#define TBOX_EXIT_USAGE     2   /* usage error / not logged in */
#define TBOX_EXIT_TDLIB     3   /* telegram/network failure (later) */
#define TBOX_EXIT_INTEGRITY 4   /* integrity/verify failure (later) */
#define TBOX_EXIT_NOT_IMPL  5   /* stub, not implemented yet (vanishes later) */
#define TBOX_EXIT_INTR      130 /* interrupted (Ctrl+C) */

typedef int (*cmd_fn)(int argc, char *argv[]);

typedef struct {
    const char *name;     /* identifier, e.g. "auth" */
    cmd_fn      fn;       /* implementation */
    const char *summary;  /* one-line description for help */
    int         exposed;  /* 1 = selectable from argv[1]; 0 = internal only */
} cmd_entry;

/*
 * Resolve argv[1] against the EXPOSED command table, run it, return its
 * exit code. Unknown or not exposed -> TBOX_EXIT_USAGE.
 */
int tbox_cmd_run(int argc, char *argv[]);

/*
 * Internal registry lookup, bypassing CLI exposure: find commands that
 * exist but are NOT reachable from argv (e.g. "selftest"). Returns NULL
 * for unknown names.
 */
cmd_fn tbox_cmd_by_name(const char *name);

/* Command implementations (stubs today; real bodies land in later steps). */
int cmd_auth(int argc, char *argv[]);
int cmd_status(int argc, char *argv[]);
int cmd_help(int argc, char *argv[]);
int cmd_version(int argc, char *argv[]);
int cmd_selftest(int argc, char *argv[]);  /* internal only, not exposed */

#endif /* TBOX_CMD_H */