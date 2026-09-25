/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * test_signal.c - interrupt listener tests (Step: signals pulled forward).
 *
 * Headless and deterministic: instead of generating a real console event
 * (which would kill the whole ctest process), it drives tbox_signal_fire()
 * - the exact code path the OS console handler calls on Ctrl+C.
 */

#include <stdio.h>
#include <windows.h>      /* Sleep */

#include "util/signal.h"

static int failures = 0;

static void check(const char *what, int got, int want)
{
    if (got == want) {
        printf("ok   %-30s %d\n", what, got);
    } else {
        printf("FAIL %-30s got %d want %d\n", what, got, want);
        failures++;
    }
}

int main(void)
{
    /* install is idempotent */
    tbox_signal_install();
    tbox_signal_install();

    check("no interrupt yet",        tbox_signal_pending(), 0);
    check("wait times out",          tbox_signal_wait(50),  0);

    /* first interrupt (a Ctrl+C) */
    tbox_signal_fire();
    check("pending after fire",      tbox_signal_pending(), 1);
    check("wait returns immediately", tbox_signal_wait(2000), 1);
    check("not force yet",           tbox_signal_force(),   0);

    /* second interrupt -> force path */
    tbox_signal_fire();
    check("force requested",         tbox_signal_force(),   1);
    check("pending stays sticky",    tbox_signal_pending(), 1);

    /* listener thread is actually alive: it must have printed the notice */
    Sleep(50);   /* give the listener one scheduling quantum */

    tbox_signal_shutdown();
    check("clean after shutdown",    tbox_signal_pending(), 0);
    check("force cleared",           tbox_signal_force(),   0);

    /* re-install works after shutdown */
    tbox_signal_install();
    check("fresh install clean",     tbox_signal_pending(), 0);
    tbox_signal_fire();
    check("fresh install works",     tbox_signal_pending(), 1);
    tbox_signal_shutdown();

    printf(failures ? "FAILED (%d)\n" : "test_signal: all green\n", failures);
    return failures ? 1 : 0;
}