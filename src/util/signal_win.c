/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * signal_win.c - Windows backend of the interrupt listener.
 *
 * Windows has no POSIX signals and no pthread: Ctrl+C is delivered by the
 * console to a dedicated per-process control-handler thread that the OS
 * starts. We register that handler (SetConsoleCtrlHandler) AND run our own
 * listener thread so "interrupted" becomes one pollable, sticky fact for
 * main / the bridge worker. All state is interlocked; the listener thread
 * never calls into TDLib (threading law).
 *
 * See tbox_signal.h for the cross-platform contract. CMake selects this file on
 * WIN32; signal_posix.c is the Linux/macOS backend.
 */

#if !defined(_WIN32)
#error "signal_win.c is the Windows backend - CMake selects it only on WIN32"
#endif

#include <windows.h>
#include <process.h>      /* _beginthreadex */
#include <stdio.h>        /* fputs */

#include "tbox_signal.h"

/* ---- shared state ------------------------------------------------------- */
static HANDLE        g_event;        /* manual-reset, signalled on interrupt */
static HANDLE        g_thread;       /* listener thread handle               */
static volatile LONG g_run;          /* 1 = listener alive                   */
static volatile LONG g_interrupted;  /* sticky: any interrupt received       */
static volatile LONG g_force;        /* sticky: second interrupt -> force    */
static volatile LONG g_notified;     /* sticky: one-line notice already shown */

/* ---- OS console handler (runs on the console's own thread) --------------- */
static BOOL WINAPI console_handler(DWORD ctrl_type)
{
    /* CTRL_C, CTRL_BREAK, CTRL_CLOSE, CTRL_LOGOFF, CTRL_SHUTDOWN all mean
     * "stop doing work". Returning TRUE = handled, no default kill. */
    (void)ctrl_type;
    tbox_signal_fire();
    return TRUE;
}

/* ---- listener thread (requested design; Step 17 hook for condvar wake) -- *
 * Pure watcher: it never consumes or clears the interrupt event (that stays
 * sticky for waiters). It blocks until an interrupt, then prints the notice
 * once. Second-interrupt (force) detection lives synchronously in fire(). */
static unsigned __stdcall listener_main(void *unused)
{
    (void)unused;
    for (;;) {
        WaitForSingleObject(g_event, INFINITE); /* parked while quiet        */

        if (InterlockedCompareExchange(&g_run, 0, 0) == 0)
            break;                              /* shutdown requested        */

        if (InterlockedCompareExchange(&g_notified, 1, 0) == 0)
            fputs("tbox: interrupt received - stopping (Ctrl+C again to force)\n",
                  stderr);
    }
    return 0;
}

/* ---- public API ---------------------------------------------------------- */

void tbox_signal_install(void)
{
    if (g_event != NULL)
        return;                                 /* idempotent               */

    g_event = CreateEventW(NULL, TRUE, FALSE, NULL); /* manual-reset, clear */
    if (g_event == NULL)
        return;                                 /* no listener: degrade     */

    InterlockedExchange(&g_run, 1);
    SetConsoleCtrlHandler(console_handler, TRUE);

    g_thread = (HANDLE)_beginthreadex(NULL, 0, listener_main, NULL, 0, NULL);
    /* If the thread cannot start, the handler still sets the event and
     * pending()/wait() keep working; only force-detect + the notice degrade. */
}

void tbox_signal_shutdown(void)
{
    if (g_event == NULL)
        return;

    SetConsoleCtrlHandler(console_handler, FALSE); /* no new interrupts     */
    InterlockedExchange(&g_run, 0);
    SetEvent(g_event);                          /* wake so it can exit      */

    if (g_thread != NULL) {
        WaitForSingleObject(g_thread, 1000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }

    CloseHandle(g_event);
    g_event = NULL;
    InterlockedExchange(&g_interrupted, 0);     /* clean slate for re-install */
    InterlockedExchange(&g_force, 0);
    InterlockedExchange(&g_notified, 0);
}

int tbox_signal_pending(void)
{
    /* Event signalled (handler fired, listener may not have woken yet) or
     * the sticky flag is set - either way an interrupt is pending. */
    if (g_event != NULL && WaitForSingleObject(g_event, 0) == WAIT_OBJECT_0)
        return 1;
    return InterlockedCompareExchange(&g_interrupted, 0, 0) != 0;
}

int tbox_signal_force(void)
{
    return InterlockedCompareExchange(&g_force, 0, 0) != 0;
}

int tbox_signal_wait(unsigned ms)
{
    if (g_event == NULL)
        return 0;
    return WaitForSingleObject(g_event, (DWORD)ms) == WAIT_OBJECT_0;
}

void tbox_signal_fire(void)
{
    const LONG was = InterlockedCompareExchange(&g_interrupted, 0, 0);
    InterlockedExchange(&g_force, was != 0);    /* 2nd interrupt -> force   */
    InterlockedExchange(&g_interrupted, 1);
    if (g_event != NULL)
        SetEvent(g_event);
}