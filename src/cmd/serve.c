/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * cmd/serve.c - `tbox serve` and `tbox status`: the ownership + visibility
 * half of the service. TDLib-free by construction - the actual session work
 * arrives through the injected runner (see cmd/cmd.h), so this file also
 * builds in a TDLib-less binary where it simply owns the lock and reports
 * that there is nothing to serve yet.
 */

#include <stdio.h>
#include <string.h>

#include "cmd.h"
#include "util/lockfile.h"
#include "util/statusfile.h"
#include "util/tbox_thread.h"

/* Path buffers: long enough for any per-OS data dir plus a file name. */
#define TBOX_PATH_MAX 512

/* The serve runner, injected by main() (see cmd.h). NULL until set. */
static tbox_serve_runner_t g_serve_runner = NULL;
static volatile bool *g_should_stop = NULL;
static bool g_tdlib_present = false;

void tbox_cmd_set_serve(tbox_serve_runner_t runner,
                        volatile bool *should_stop)
{
    g_serve_runner = runner;
    g_should_stop = should_stop;
}

void tbox_cmd_set_tdlib_present(bool present)
{
    g_tdlib_present = present;
}

/* ---- serve ---------------------------------------------------------------- */

/*
 * Publish a status state. Failure to write is reported but never fatal: the
 * lock is what protects the session, status.json is only observability (and
 * `tbox status` will say "stale" if it goes missing).
 */
static int publish(const char *status_path, tbox_status_t *status,
                   tbox_state_t state, const char *error)
{
    status->state = state;
    status->error[0] = '\0';
    if (error != NULL) {
        size_t len = strlen(error);

        if (len >= sizeof status->error)
            len = sizeof status->error - 1;
        memcpy(status->error, error, len + 1);
    }

    if (tbox_status_write(status_path, status) != 0) {
        fprintf(stderr, "serve: could not update %s\n", status_path);
        return -1;
    }

    return 0;
}

int tbox_cmd_serve(int argc, char *argv[])
{
    char root[TBOX_PATH_MAX];
    char lock_path[TBOX_PATH_MAX];
    char status_path[TBOX_PATH_MAX];
    const char *data_dir;
    tbox_lockfile_t lock;
    tbox_status_t status;
    long owner = 0;
    int rc;

    if (tbox_cmd_parse_data(argc, argv, &data_dir) != TBOX_EXIT_OK)
        return TBOX_UNKNOWN_COMMAND;

    if (tbox_cmd_resolve_root("serve", data_dir, root, sizeof root, 1) != 0)
        return TBOX_ERROR;

    if (tbox_lockfile_path(lock_path, sizeof lock_path, root) != 0
        || tbox_status_path(status_path, sizeof status_path, root) != 0) {
        fprintf(stderr, "serve: data path is too long.\n");
        return TBOX_ERROR;
    }

    /* Ownership first: one Telegram session, one owner. This is the
     * AuthKeyUnregisteredError lesson from v0.3 - two processes on one
     * session invalidate the auth key within minutes. */
    rc = tbox_lockfile_acquire(&lock, lock_path);
    if (rc == -1) {
        if (tbox_lockfile_read_pid(lock_path, &owner) == 0)
            fprintf(stderr, "serve: already running (pid %ld).\n", owner);
        else
            fprintf(stderr, "serve: already running.\n");
        return TBOX_ERROR;
    }
    if (rc != 0) {
        fprintf(stderr, "serve: could not open %s\n", lock_path);
        return TBOX_ERROR;
    }

    tbox_status_init(&status);
    (void)publish(status_path, &status, TBOX_STATE_STARTING, NULL);

    printf("serve: owning the session in %s\n", root);

    if (g_serve_runner == NULL) {
        /* No runner: the lock is still held, so a real `auth` or a second
         * `serve` cannot touch the session - and be honest about *why* there
         * is nothing to serve. */
        if (g_tdlib_present)
            printf("serve: Telegram support is compiled in but the serve "
                   "runner was not wired up - that is a build bug.\n");
        else
            printf("serve: this build has no Telegram support "
                   "(run scripts/fetch-tdjson.* and rebuild).\n");
        printf("serve: owning the session anyway, serving nothing.\n");
        (void)publish(status_path, &status, TBOX_STATE_STOPPED, NULL);
        tbox_lockfile_release(&lock);
        return TBOX_ERROR;
    }

    /* The runner owns the session from here: it scans the archive, starts a
     * transport and publishes every state change itself, including the
     * terminal one - only it knows the endpoint and the real entry count, so
     * publishing over it here would report "no transport" for a live run. */
    rc = g_serve_runner(root, g_should_stop);

    tbox_lockfile_release(&lock);

    return rc == 0 ? TBOX_EXIT_OK : TBOX_ERROR;
}

/* ---- status --------------------------------------------------------------- */

int tbox_cmd_status(int argc, char *argv[])
{
    char root[TBOX_PATH_MAX];
    char status_path[TBOX_PATH_MAX];
    const char *data_dir;
    tbox_status_t status;

    if (tbox_cmd_parse_data(argc, argv, &data_dir) != TBOX_EXIT_OK)
        return TBOX_UNKNOWN_COMMAND;

    /* read-only: never creates the directory it asks about */
    if (tbox_cmd_resolve_root("status", data_dir, root, sizeof root, 0) != 0)
        return TBOX_ERROR;

    if (tbox_status_path(status_path, sizeof status_path, root) != 0) {
        fprintf(stderr, "status: data path is too long.\n");
        return TBOX_ERROR;
    }

    /* Reads the file only - never opens TDLib, so this works while serve is
     * running, and also after it died (the pid then says "stale"). */
    if (tbox_status_read(status_path, &status) != 0) {
        printf("status: not running (no status file in %s)\n", root);
        return TBOX_ERROR;
    }

    printf("state      %s\n", tbox_state_name(status.state));
    printf("data dir   %s\n", root);
    if (status.user[0] != '\0')
        printf("account    %s %s\n", status.user, status.phone);
    if (status.transport[0] != '\0')
        printf("transport  %s at %s\n", status.transport, status.endpoint);
    else
        printf("transport  (none)\n");
    printf("indexed    %lld entries\n", status.indexed);
    printf("cache      %lld bytes\n", status.cache_bytes);
    printf("since      %s\n", status.since);
    if (status.error[0] != '\0')
        printf("error      %s\n", status.error);

    if (tbox_state_is_running(status.state)) {
        switch (tbox_pid_alive(status.pid)) {
        case 1:
            printf("owner      pid %ld is running\n", status.pid);
            break;
        case 0:
            /* The file outlived its process: a crash or a kill -9. */
            printf("owner      pid %ld is gone - stale status\n",
                   status.pid);
            break;
        default:
            printf("owner      unknown\n");
            break;
        }
    }

    if (!tbox_state_is_running(status.state)) {
        printf("status: not running\n");
        return TBOX_ERROR;
    }

    return TBOX_EXIT_OK;
}