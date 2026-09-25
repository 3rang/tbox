/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * test_signal.c - tests for src/util/tbox_signal.h.
 *
 * The module provides:
 *   - a cross-platform OS thread wrapper (tbox_os_thread_create/join/close)
 *   - an OS event API (tbox_os_event_start/stop/received) that normalizes
 *     Ctrl+C / Ctrl+Break / SIGTERM / SIGHUP etc. into tbox_os_event_t
 * These tests exercise that contract with the same file on both platforms.
 * Headless by rule - no TDLib, no network.
 */

#include <stdio.h>
#include <signal.h>
#include "util/tbox_signal.h"

static int failures = 0;

static void check(const char *what, int got, int want)
{
    if (got == want) {
        printf("ok   %-36s got %d\n", what, got);
    } else {
        printf("FAIL %-36s got %d want %d\n", what, got, want);
        failures++;
    }
}

/* ---- thread API ----------------------------------------------------------- */

static void *worker_set(void *arg)
{
    int *p = (int *)arg;
    *p = 42;
    return NULL;
}

static void test_threads(void)
{
    tbox_os_thread_t th;
    int val = 0;

    /* create -> worker runs on its own thread -> join -> close */
    check("thread: create", tbox_os_thread_create(&th, worker_set, &val), 0);
    check("thread: join", tbox_os_thread_join(&th), 0);
    tbox_os_thread_close(&th);
    check("thread: worker ran with arg", val, 42);

    /* invalid arguments -> -1, no thread started */
    check("thread: create NULL func", tbox_os_thread_create(&th, NULL, NULL), -1);
    check("thread: create NULL thread", tbox_os_thread_create(NULL, worker_set, NULL), -1);

    /* a second round trip stays healthy (no leaked state) */
    val = 0;
    check("thread: create again", tbox_os_thread_create(&th, worker_set, &val), 0);
    check("thread: join again", tbox_os_thread_join(&th), 0);
    tbox_os_thread_close(&th);
    check("thread: second worker ran", val, 42);
}

/* ---- event API ------------------------------------------------------------ */

static tbox_os_event_t g_last_event = OS_EVENT_NONE;

static void record_callback(tbox_os_event_t event)
{
    g_last_event = event;
}

static void test_events(void)
{
    /* lifecycle: start -> no event -> stop, all clean */
    check("event: start", tbox_os_event_start(record_callback), 0);
    check("event: none received", tbox_os_event_received(), 0);
    check("event: still none", tbox_os_event_received(), 0);
    tbox_os_event_stop();

    /* NULL callback is accepted; nothing to invoke */
    check("event: start NULL cb", tbox_os_event_start(NULL), 0);
    check("event: none received (NULL cb)", tbox_os_event_received(), 0);
    tbox_os_event_stop();

    /* restart is idempotent */
    check("event: restart 1", tbox_os_event_start(record_callback), 0);
    tbox_os_event_stop();
    check("event: restart 2", tbox_os_event_start(record_callback), 0);
    tbox_os_event_stop();

#if defined(OS_PLATFORM_POSIX)
    /* positive path (POSIX only): a real SIGINT reaches received() once,
     * the callback sees the normalized event, and the event is cleared. */
    g_last_event = OS_EVENT_NONE;
    check("event: start (posix)", tbox_os_event_start(record_callback), 0);
    raise(SIGINT);
    check("event: received after SIGINT", tbox_os_event_received(), 1);
    check("event: callback got INTERRUPT", g_last_event == OS_EVENT_INTERRUPT, 1);
    check("event: consumed (cleared)", tbox_os_event_received(), 0);
    tbox_os_event_stop();
#endif
}

int main(void)
{
    test_threads();
    test_events();

    printf(failures ? "FAILED (%d)\n" : "test_signal: all green\n", failures);
    return failures ? 1 : 0;
}