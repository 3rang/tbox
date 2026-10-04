/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * test_statusfile.c - the service seam (util/statusfile.c, util/lockfile.c,
 * util/datadir.c).
 *
 * These three decide whether a second `tbox serve` can hijack a live
 * Telegram session, and whether `tbox status` can be trusted. They are all
 * pure local file work, so they stay headless: no TDLib, no network, no
 * secrets, no real data directory (scratch files next to the test binary,
 * removed at the end).
 */

#define _CRT_SECURE_NO_WARNINGS   /* MSVC: remove/rename in a fixed cwd */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/datadir.h"
#include "util/lockfile.h"
#include "util/statusfile.h"

static int failures = 0;

static void check(const char *what, long long got, long long want)
{
    if (got == want) {
        printf("ok   %-46s %lld\n", what, got);
    } else {
        printf("FAIL %-46s got %lld want %lld\n", what, got, want);
        failures++;
    }
}

static void check_str(const char *what, const char *got, const char *want)
{
    if (got != NULL && want != NULL && strcmp(got, want) == 0) {
        printf("ok   %-46s %s\n", what, got);
    } else {
        printf("FAIL %-46s got \"%s\" want \"%s\"\n", what,
               got != NULL ? got : "(null)", want != NULL ? want : "(null)");
        failures++;
    }
}

#define STATUS_FILE "test_statusfile.scratch.json"
#define LOCK_FILE   "test_statusfile.scratch.lock"
#define TMP_FILE    "test_statusfile.scratch.json.tmp"

/* ---- state names ---------------------------------------------------------- */

static void test_states(void)
{
    static const tbox_state_t states[] = {
        TBOX_STATE_STARTING, TBOX_STATE_QR_REQUIRED, TBOX_STATE_AUTHORIZING,
        TBOX_STATE_READY, TBOX_STATE_STOPPED, TBOX_STATE_ERROR
    };
    static const char *names[] = {
        "starting", "qr_required", "authorizing", "ready", "stopped", "error"
    };
    size_t i;

    for (i = 0; i < sizeof states / sizeof states[0]; i++) {
        check_str("state: wire name", tbox_state_name(states[i]), names[i]);
        check("state: round-trip",
              tbox_state_from_name(names[i]), (long long)states[i]);
        check("state: running?", tbox_state_is_running(states[i]),
              i < 4 ? 1 : 0);            /* the first four mean "alive" */
    }

    check_str("state: unknown has a name",
              tbox_state_name(TBOX_STATE_UNKNOWN), "unknown");
    check("state: NULL -> unknown", tbox_state_from_name(NULL),
          TBOX_STATE_UNKNOWN);
    check("state: garbage -> unknown", tbox_state_from_name("halfway"),
          TBOX_STATE_UNKNOWN);
    check("state: case sensitive", tbox_state_from_name("READY"),
          TBOX_STATE_UNKNOWN);
    check("state: zero value is unknown", (long long)TBOX_STATE_UNKNOWN, 0);
}

/* ---- timestamps ----------------------------------------------------------- */

static void test_timestamps(void)
{
    char buf[32];

    tbox_status_now(buf, sizeof buf);
    check("since: length", (long long)strlen(buf), 20);
    check("since: is UTC (Z suffix)", buf[19] == 'Z', 1);
    check("since: has date separator", buf[4] == '-', 1);
    check("since: has time separator", buf[10] == 'T', 1);
    check("since: has time separator", buf[13] == ':', 1);
    check("since: digits where expected",
          buf[0] >= '0' && buf[0] <= '9', 1);

    /* a zero-sized buffer must not write at all */
    buf[0] = 'x';
    tbox_status_now(buf, 0);
    check("since: tiny buffer untouched", buf[0] == 'x', 1);
}

/* ---- write / read round-trip --------------------------------------------- */

static void test_roundtrip(void)
{
    tbox_status_t written;
    tbox_status_t read_back;

    (void)remove(STATUS_FILE);

    tbox_status_init(&written);
    check("write: initial state", written.state, TBOX_STATE_STARTING);
    check("write: pid is ours", written.pid, tbox_pid_self());

    strcpy(written.user, "tpat");
    strcpy(written.phone, "+919999999999");
    strcpy(written.transport, "ftp");
    strcpy(written.endpoint, "127.0.0.1:2121");
    written.indexed = 128;
    written.cache_bytes = 1048576;

    check("write: ready status stored",
          tbox_status_write(STATUS_FILE, &written), 0);
    check("write: file exists", tbox_status_read(STATUS_FILE, &read_back), 0);

    check("read: state", read_back.state, TBOX_STATE_STARTING);
    check_str("read: user", read_back.user, "tpat");
    check_str("read: phone", read_back.phone, "+919999999999");
    check_str("read: transport", read_back.transport, "ftp");
    check_str("read: endpoint", read_back.endpoint, "127.0.0.1:2121");
    check("read: indexed", read_back.indexed, 128);
    check("read: cache_bytes", read_back.cache_bytes, 1048576);
    check("read: pid", read_back.pid, tbox_pid_self());
    check_str("read: error is empty", read_back.error, "");
    check_str("read: since survives", read_back.since, written.since);

    check("read: state", read_back.state, TBOX_STATE_STARTING);

    /* an error message round-trips */
    written.state = TBOX_STATE_ERROR;
    strcpy(written.error, "TDLib: network is unreachable");
    check("write: error status stored",
          tbox_status_write(STATUS_FILE, &written), 0);
    check("write: replaced", tbox_status_read(STATUS_FILE, &read_back), 0);
    check("read: error state", read_back.state, TBOX_STATE_ERROR);
    check_str("read: error text", read_back.error,
              "TDLib: network is unreachable");

    /* a second write must replace the file atomically, not append */
    written.state = TBOX_STATE_READY;
    written.error[0] = '\0';
    written.indexed = 129;
    check("write: replaced again",
          tbox_status_write(STATUS_FILE, &written), 0);
    check("write: replaced again readable",
          tbox_status_read(STATUS_FILE, &read_back), 0);
    check("read: newest indexed wins", read_back.indexed, 129);
    check_str("read: newest error cleared", read_back.error, "");
    check("write: no temp file left behind",
          tbox_status_read(TMP_FILE, &read_back), -1);
}

/* ---- read failures -------------------------------------------------------- */

static void test_read_failures(void)
{
    tbox_status_t status;
    FILE *fp;

    check("read: missing file", tbox_status_read("no_such_status.json", &status), -1);

    fp = fopen(STATUS_FILE, "wb");
    if (fp != NULL) {
        (void)fputs("{\"v\":1,\"state\":\"ready\"}", fp);
        (void)fclose(fp);
        check("read: partial json still parses", tbox_status_read(STATUS_FILE, &status), 0);
        check("read: missing fields default", status.indexed, 0);
        check_str("read: missing user is empty", status.user, "");
        check("read: missing pid is 0", status.pid, 0);
    }

    fp = fopen(STATUS_FILE, "wb");
    if (fp != NULL) {
        (void)fputs("{ truncated", fp);       /* a half-written file */
        (void)fclose(fp);
        check("read: truncated file rejected",
              tbox_status_read(STATUS_FILE, &status), -1);
    }

    fp = fopen(STATUS_FILE, "wb");
    if (fp != NULL) {
        (void)fputs("{\"v\":99,\"state\":\"ready\"}", fp);   /* wrong version */
        (void)fclose(fp);
        check("read: wrong version rejected",
              tbox_status_read(STATUS_FILE, &status), -1);
    }

    fp = fopen(STATUS_FILE, "wb");
    if (fp != NULL) {
        (void)fputs("[1,2,3]", fp);            /* valid JSON, wrong shape */
        (void)fclose(fp);
        check("read: array rejected", tbox_status_read(STATUS_FILE, &status), -1);
    }

    /* an unknown state must not be mistaken for "running" */
    fp = fopen(STATUS_FILE, "wb");
    if (fp != NULL) {
        (void)fputs("{\"v\":1,\"state\":\"weird\"}", fp);
        (void)fclose(fp);
        check("read: unknown state parses", tbox_status_read(STATUS_FILE, &status), 0);
        check("read: unknown state value", status.state, TBOX_STATE_UNKNOWN);
        check("read: unknown state is not running",
              tbox_state_is_running(status.state), 0);
    }

    check("write: NULL path", tbox_status_write(NULL, NULL), -1);
    check("read: NULL path", tbox_status_read(NULL, &status), -1);
    check("read: over-long path is refused",
          tbox_status_read("0123456789012345678901234567890123456789"
                           "0123456789012345678901234567890123456789"
                           "0123456789012345678901234567890123456789"
                           "0123456789012345678901234567890123456789"
                           "0123456789012345678901234567890123456789"
                           "0123456789/status.json", &status), -1);
}

/* ---- the exclusive lock --------------------------------------------------- */

static void test_lock(void)
{
    tbox_lockfile_t first;
    tbox_lockfile_t second;
    long pid = 0;

    (void)remove(LOCK_FILE);

    check("lock: acquire", tbox_lockfile_acquire(&first, LOCK_FILE), 0);
    check("lock: marked held", first.held, 1);
    check("lock: pid recorded", first.pid, tbox_pid_self());

    /* the second acquire must be refused: two owners of one Telegram
     * session is exactly what destroys the auth key */
    check("lock: second acquire refused",
          tbox_lockfile_acquire(&second, LOCK_FILE), -1);
    check("lock: second is not held", second.held, 0);

    /* a reader still gets in, so the error message can name the owner */
    check("lock: owner pid readable", tbox_lockfile_read_pid(LOCK_FILE, &pid), 0);
    check("lock: pid is ours", pid, tbox_pid_self());

    tbox_lockfile_release(&first);
    check("lock: released", first.held, 0);

    /* after release the archive is servable again - no stale lock */
    check("lock: reacquire after release",
          tbox_lockfile_acquire(&second, LOCK_FILE), 0);
    tbox_lockfile_release(&second);

    /* releasing twice is harmless */
    tbox_lockfile_release(&second);
    check("lock: double release is safe", second.held, 0);

    (void)remove(LOCK_FILE);
    check("lock: missing file has no pid",
          tbox_lockfile_read_pid(LOCK_FILE, &pid), -1);
    check("lock: NULL path rejected",
          tbox_lockfile_acquire(&first, NULL), -2);
}

/* ---- process identity ----------------------------------------------------- */

static void test_pid(void)
{
    check("pid: self is positive", tbox_pid_self() > 0, 1);
    check("pid: we are alive", tbox_pid_alive(tbox_pid_self()), 1);
    check("pid: 0 is not meaningful", tbox_pid_alive(0), -1);
    check("pid: negative is not meaningful", tbox_pid_alive(-5), -1);
}

/* ---- path helpers --------------------------------------------------------- */

/* The platform separator in a path built from "root" + separator + leaf. */
static const char *expected(const char *root, const char *leaf)
{
    static char buf[512];

    snprintf(buf, sizeof buf, "%s%c%s", root,
#ifdef _WIN32
             '\\',
#else
             '/',
#endif
             leaf);

    return buf;
}

static void test_paths(void)
{
    char buf[512];

    check("path: lock file name",
          tbox_lockfile_path(buf, sizeof buf, "/data/tbox") == 0
              && strcmp(buf, expected("/data/tbox", "session.lock")) == 0, 1);
    check("path: status file name",
          tbox_status_path(buf, sizeof buf, "/data/tbox") == 0
              && strcmp(buf, expected("/data/tbox", "status.json")) == 0, 1);
    /* a root that already ends in a separator keeps that one, so the result
     * is the same string with either separator style */
    check("path: trailing separator is not doubled",
          tbox_lockfile_path(buf, sizeof buf, "/data/tbox/") == 0
              && strcmp(buf, "/data/tbox/session.lock") == 0, 1);
    #ifdef _WIN32
    /* Windows tolerates "/" in place of "\", so a root ending in either
     * separator must not gain a second one. On POSIX "\" is an ordinary
     * filename character, so there is nothing to check here. */
    check("path: a backslash root is not doubled either",
          tbox_lockfile_path(buf, sizeof buf, "C:\\data\\tbox\\") == 0
              && strcmp(buf, expected("C:\\data\\tbox", "session.lock")) == 0, 1);
    check("path: a mixed-separator root keeps one separator",
          tbox_status_path(buf, sizeof buf, "C:/data/tbox") == 0
              && strcmp(buf, expected("C:/data/tbox", "status.json")) == 0, 1);
#endif
    check("path: session dir",
          tbox_datadir_session(buf, sizeof buf, "/data/tbox") == 0
              && strcmp(buf, expected("/data/tbox", "tdlib")) == 0, 1);

    /* join must fail loudly rather than truncate a path */
    check("path: over-long join refused",
          tbox_datadir_join(buf, 8, "/data/tbox", "session.lock"), -1);
    check("path: NULL root refused",
          tbox_datadir_join(buf, sizeof buf, NULL, "x"), -1);

    /* the per-OS default has to resolve, or nothing else can start */
    check("path: default root resolves",
          tbox_datadir_default(buf, sizeof buf), 0);
    check("path: default root is not empty", buf[0] != '\0', 1);
    check("path: default root has a leaf",
          strstr(buf, "tbox") != NULL, 1);
}

static void cleanup(void)
{
    (void)remove(STATUS_FILE);
    (void)remove(TMP_FILE);
    (void)remove(LOCK_FILE);
}

int main(void)
{
    test_states();
    test_timestamps();
    test_roundtrip();
    test_read_failures();
    test_lock();
    test_pid();
    test_paths();

    cleanup();

    printf(failures ? "FAILED (%d)\n" : "test_statusfile: all green\n", failures);
    return failures ? 1 : 0;
}