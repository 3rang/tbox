/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * signal_win.c - Windows backend of the interrupt listener.
 *
 */
#include "tbox_signal.h"
#include <stdlib.h>
#include <windows.h>

static tbox_os_event_func_t g_callback = NULL;

static volatile LONG g_event = OS_EVENT_NONE;


/*
 * Windows console control handler.
 *
 * Called by Windows when:
 *
 * Ctrl+C
 * Ctrl+Break
 * Console close
 * Logoff
 * Shutdown
 */
static BOOL WINAPI os_console_handler(DWORD signal)
{
    switch (signal) {

        case CTRL_C_EVENT:
            InterlockedExchange(
                &g_event,
                OS_EVENT_INTERRUPT
            );
            return TRUE;

        case CTRL_BREAK_EVENT:
            InterlockedExchange(
                &g_event,
                OS_EVENT_BREAK
            );
            return TRUE;

        case CTRL_CLOSE_EVENT:
            InterlockedExchange(
                &g_event,
                OS_EVENT_TERMINATE
            );
            return TRUE;

        case CTRL_LOGOFF_EVENT:
            InterlockedExchange(
                &g_event,
                OS_EVENT_LOGOFF
            );
            return TRUE;

        case CTRL_SHUTDOWN_EVENT:
            InterlockedExchange(
                &g_event,
                OS_EVENT_SHUTDOWN
            );
            return TRUE;

        default:
            return FALSE;
    }
}


int tbox_os_event_start(tbox_os_event_func_t callback)
{
    g_callback = callback;

    InterlockedExchange(
        &g_event,
        OS_EVENT_NONE
    );

    if (!SetConsoleCtrlHandler(
            os_console_handler,
            TRUE)) {

        return -1;
    }

    return 0;
}


void tbox_os_event_stop(void)
{
    SetConsoleCtrlHandler(
        os_console_handler,
        FALSE
    );

    g_callback = NULL;
}


int tbox_os_event_received(void)
{
    tbox_os_event_t event;

    event = (tbox_os_event_t)InterlockedExchange(
        &g_event,
        OS_EVENT_NONE
    );

    if (event == OS_EVENT_NONE)
        return 0;

    if (g_callback != NULL)
        g_callback(event);

    return 1;
}

/* ---- thread API ----------------------------------------------------------- */

typedef struct {
    tbox_os_thread_func_t func;
    void *arg;
} os_thread_context_t;

static DWORD WINAPI os_thread_entry(LPVOID arg)
{
    os_thread_context_t *ctx = arg;
    ctx->func(ctx->arg);
    free(ctx);
    return 0;
}


int tbox_os_thread_create(tbox_os_thread_t *thread,
                     tbox_os_thread_func_t func,
                     void *arg)
{
    if (!thread || !func) {
        return -1;
    }

    os_thread_context_t *ctx = malloc(sizeof(os_thread_context_t));
    if (!ctx) {
        return -1;
    }
    ctx->func = func;
    ctx->arg = arg;

    thread->handle = CreateThread(NULL, 0, os_thread_entry, ctx, 0, NULL);

    if (!thread->handle) {
        free(ctx);
        return -1;
    }

    return 0;
}

int tbox_os_thread_join(tbox_os_thread_t *thread)
{
    DWORD result;
    result = WaitForSingleObject(thread->handle, INFINITE);
    return (result == WAIT_OBJECT_0) ? 0 : -1;
}

void tbox_os_thread_close(tbox_os_thread_t *thread)
{
    if (thread->handle != NULL) {
        CloseHandle(thread->handle);
        thread->handle = NULL;
    }
}