/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * test_signal.c - tests for src/util/tbox_signal.h.
 *
 * The module runs a dedicated signal thread (SetConsoleCtrlHandler on
 * Windows, sigwait() on POSIX) that normalizes OS signals into
 * tbox_signal_t and dispatches them to an application callback. These
 * tests exercise that contract with the same file on both platforms.
 * Headless by rule - no TDLib, no network.
 */

#include <stdio.h>
#include <signal.h>
#include "util/tbox_signal.h"

#if defined(OS_SIGNAL_POSIX)
#include <unistd.h>   /* getpid, usleep */
#include <sys/types.h>
#endif

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

static volatile tbox_signal_t g_last = OS_SIGNAL_NONE;

static void record_callback(tbox_signal_t signal)
{
    g_last = signal;
}

static void test_bad_args(void)
{
    tbox_signal_thread_t th;

    check("start NULL thread", tbox_os_signal_start(NULL, record_callback), -1);
    check("start NULL callback", tbox_os_signal_start(&th, NULL), -1);
}

static void test_lifecycle(void)
{
    tbox_signal_thread_t th;

    check("start", tbox_os_signal_start(&th, record_callback), 0);
    check("stop", tbox_os_signal_stop(&th), 0);

    /* restart works: resources are fully released by stop */
    check("start again", tbox_os_signal_start(&th, record_callback), 0);
    check("stop again", tbox_os_signal_stop(&th), 0);
}

#if defined(OS_SIGNAL_POSIX)
static void test_posix_signal(void)
{
    tbox_signal_thread_t th;
    int spins = 0;

    /*
     * tbox_os_signal_start() blocks SIGINT in this thread; the signal
     * thread accepts it via sigwait(). kill(getpid(), ...) sends a
     * process-directed signal, which is what sigwait() in another thread
     * can receive (a plain raise() would be thread-directed and stuck in
     * this thread). Bounded spin: the callback runs on the signal thread.
     */
    g_last = OS_SIGNAL_NONE;

    check("posix: start", tbox_os_signal_start(&th, record_callback), 0);

    kill(getpid(), SIGINT);

    while (g_last == OS_SIGNAL_NONE && spins < 5000)
        { usleep(1000); spins++; }

    check("posix: callback got INTERRUPT",
          g_last == OS_SIGNAL_INTERRUPT, 1);
    check("posix: stop", tbox_os_signal_stop(&th), 0);
}
#endif

int main(void)
{
    test_bad_args();
    test_lifecycle();
#if defined(OS_SIGNAL_POSIX)
    test_posix_signal();
#endif

    printf(failures ? "FAILED (%d)\n" : "test_signal: all green\n", failures);
    return failures ? 1 : 0;
}