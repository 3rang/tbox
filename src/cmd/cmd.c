/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#include <stdio.h>
#include <string.h>

#include "cmd.h"
#include "util/tbox_thread.h"

#ifndef TBOX_VERSION
#define TBOX_VERSION "0.5.0"
#endif

/* Auth wiring, injected by main() (see cmd.h): keeps tboxcore + the
 * headless tests TDLib-free. NULL until set -> `auth` reports itself
 * unavailable in this build. */
static tbox_auth_runner_t g_auth_runner = NULL;
static volatile bool *g_should_stop = NULL;

void tbox_cmd_set_auth(tbox_auth_runner_t runner,
                       volatile bool *should_stop)
{
    g_auth_runner = runner;
    g_should_stop = should_stop;
}

/* ---- auth command -------------------------------------------------------- */

/* One run's context; lives on the starting thread's stack, so the worker
 * must be joined before this goes out of scope. */
typedef struct
{
    tbox_auth_runner_t runner;
    const char *data_dir;            /* NULL = per-OS default */
    volatile bool *should_stop;      /* shared cancel flag (may be NULL) */
    volatile bool done;
    int code;

} auth_job_t;

static void *auth_worker(void *arg)
{
    auth_job_t *job = arg;

    job->code = job->runner(job->data_dir, job->should_stop);
    job->done = true;

    return NULL;
}

/* usage: tbox auth [--data <dir>] */
int tbox_cmd_auth(int argc, char *argv[])
{
    const char *data_dir;
    auth_job_t job;
    tbox_thread_t thread;

    if (tbox_cmd_parse_data(argc, argv, &data_dir) != TBOX_EXIT_OK)
        return TBOX_UNKNOWN_COMMAND;

    if (g_auth_runner == NULL) {
        fprintf(stderr, "auth: TDLib login not available in this build.\n");
        return TBOX_ERROR;
    }

    memset(&job, 0, sizeof job);
    job.runner = g_auth_runner;
    job.data_dir = data_dir;
    job.should_stop = g_should_stop;

    printf("tbox auth: starting QR login (Ctrl+C cancels)\n");

    if (tbox_thread_start(&thread, auth_worker, &job) != 0) {
        fprintf(stderr, "auth: could not start the login thread.\n");
        return TBOX_ERROR;
    }

    /* Side-by-side model: this thread waits while the signal listener
     * keeps running; cancellation flows through job.should_stop. */
    while (!job.done)
        tbox_sleep_ms(100);

    if (tbox_thread_join(&thread) != 0)
        return TBOX_ERROR;

    switch (job.code) {
    case 0:
        printf("auth: login complete\n");
        return TBOX_EXIT_OK;
    case 1:
        printf("auth: cancelled\n");
        return TBOX_ERROR;
    default:
        fprintf(stderr, "auth: login failed\n");
        return TBOX_ERROR;
    }
}

/* ---- version / help ------------------------------------------------------- */

void tbox_cmd_version(void)
{
    printf("tbox %s\n", TBOX_VERSION);
}

void tbox_cmd_help(void)
{
    printf("tbox - terminal Telegram box (C17 + TDLib)\n");
    printf("\n");
    printf("usage: tbox <command> [args]\n");
    printf("\n");
    printf("commands:\n");
    printf("  auth [--data <dir>]     log in via QR code (QR-only)\n");
    printf("  serve [--data <dir>]    own the session and serve the archive\n");
    printf("  status [--data <dir>]   show serve state (reads status.json only)\n");
    printf("  selftest                check the core rules offline\n");
    printf("  help                    show this help\n");
    printf("\n");
    printf("flags:\n");
    printf("  -v, --version   print version\n");
    printf("  -h, --help      show this help\n");
    printf("\n");
    printf("--data selects the state directory; it defaults to\n");
    printf("%%LOCALAPPDATA%%\\tbox on Windows and $XDG_DATA_HOME/tbox elsewhere.\n");
    printf("\n");
    printf("exit codes: %d ok, %d error, %d unknown command.\n",
           TBOX_EXIT_OK, TBOX_ERROR, TBOX_UNKNOWN_COMMAND);
}
