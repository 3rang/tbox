/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * test_thread.c - tests for src/util/tbox_thread.h.
 *
 * The wrapper runs one callable on a fresh OS thread (CreateThread on
 * Windows, pthread_create on POSIX) and joins it. tbox auth uses it to
 * run the TDLib login while the signal thread listens, with a shared
 * volatile flag for cooperative cancellation. These tests exercise that
 * contract. Headless by rule - no TDLib, no network.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "util/tbox_thread.h"

static int failures = 0;

static void check(const char *what, int got, int want)
{
    if (got == want) {
        printf("ok   %-40s got %d\n", what, got);
    } else {
        printf("FAIL %-40s got %d want %d\n", what, got, want);
        failures++;
    }
}

/* ---- worker used by the lifecycle/cancel tests ---- */

static volatile int g_ran;
static volatile bool g_stop;

static void *counting_worker(void *arg)
{
    int *out = arg;

    g_ran = 1;
    *out = 42;                 /* result lives in caller-owned memory */
    return NULL;
}

static void *waiting_worker(void *arg)
{
    int *out = arg;
    int spins = 0;

    while (!g_stop && spins < 10000) {  /* bounded: never blocks forever */
        tbox_sleep_ms(1);
        spins++;
    }
    *out = g_stop ? 7 : 8;
    return NULL;
}

/* ---- tests ---- */

static void test_bad_args(void)
{
    tbox_thread_t th;
    int x = 0;

    memset(&th, 0, sizeof th);   /* started = 0 -> join must refuse */

    check("start NULL thread", tbox_thread_start(NULL, counting_worker, &x), -1);
    check("start NULL fn", tbox_thread_start(&th, NULL, &x), -1);
    check("join never started", tbox_thread_join(&th), -1);
}

static void test_lifecycle(void)
{
    tbox_thread_t th;
    int result = 0;

    g_ran = 0;
    check("start", tbox_thread_start(&th, counting_worker, &result), 0);
    check("join", tbox_thread_join(&th), 0);
    check("worker ran", g_ran, 1);
    check("worker wrote result", result, 42);

    /* restart works: resources are fully released by join */
    check("start again", tbox_thread_start(&th, counting_worker, &result), 0);
    check("join again", tbox_thread_join(&th), 0);
    check("result again", result, 42);
}

static void test_cooperative_cancel(void)
{
    tbox_thread_t th;
    int result = 0;

    /* the tbox auth pattern: worker polls a shared flag until the
     * signal side (here the test) flips it */
    g_stop = false;
    check("cancel: start", tbox_thread_start(&th, waiting_worker, &result), 0);
    tbox_sleep_ms(20);
    g_stop = true;
    check("cancel: join", tbox_thread_join(&th), 0);
    check("cancel: worker saw the flag", result, 7);
}

static void test_sleep(void)
{
    tbox_sleep_ms(1);          /* must return, must not crash */
    printf("ok   tbox_sleep_ms returns\n");
}

int main(void)
{
    test_bad_args();
    test_lifecycle();
    test_cooperative_cancel();
    test_sleep();

    printf(failures ? "FAILED (%d)\n" : "test_thread: all green\n", failures);
    return failures ? 1 : 0;
}
