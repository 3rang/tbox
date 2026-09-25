/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * test_signal.c - tests for src/util/tbox_signal.h.
 *
 * The module currently provides the cross-platform OS thread wrapper
 * (tbox_os_thread_create/join/close) over the native Win32/POSIX APIs.
 * These tests exercise that contract with the same file on both platforms.
 * Headless by rule - no TDLib, no network.
 */

#include <stdio.h>
#include "util/tbox_signal.h"

static int failures = 0;

static void check(const char *what, int got, int want)
{
    if (got == want) {
        printf("ok   %-32s got %d\n", what, got);
    } else {
        printf("FAIL %-32s got %d want %d\n", what, got, want);
        failures++;
    }
}

/* worker: writes through its argument, returns a marker */
static void *worker_set(void *arg)
{
    int *p = (int *)arg;
    *p = 42;
    return NULL;
}

int main(void)
{
    tbox_os_thread_t th;
    int val = 0;

    /* create -> worker runs on its own thread -> join -> close */
    check("create", tbox_os_thread_create(&th, worker_set, &val), 0);
    check("join", tbox_os_thread_join(&th), 0);
    tbox_os_thread_close(&th);
    check("worker ran with arg", val, 42);

    /* invalid arguments -> -1, no thread started */
    check("create NULL func", tbox_os_thread_create(&th, NULL, NULL), -1);
    check("create NULL thread", tbox_os_thread_create(NULL, worker_set, NULL), -1);

    /* a second round trip stays healthy (no leaked state) */
    val = 0;
    check("create again", tbox_os_thread_create(&th, worker_set, &val), 0);
    check("join again", tbox_os_thread_join(&th), 0);
    tbox_os_thread_close(&th);
    check("second worker ran", val, 42);

    printf(failures ? "FAILED (%d)\n" : "test_signal: all green\n", failures);
    return failures ? 1 : 0;
}