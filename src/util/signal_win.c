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



static tbox_signal_callback_t g_callback = NULL;

static HANDLE g_signal_event = NULL;

static volatile LONG g_signal_type = OS_SIGNAL_NONE;


static BOOL WINAPI os_console_handler(DWORD signal)
{
    tbox_signal_t event;

    switch (signal) {

    case CTRL_C_EVENT:
        event = OS_SIGNAL_INTERRUPT;
        break;

    case CTRL_BREAK_EVENT:
        event = OS_SIGNAL_BREAK;
        break;

    case CTRL_CLOSE_EVENT:
        event = OS_SIGNAL_TERMINATE;
        break;

    case CTRL_LOGOFF_EVENT:
        event = OS_SIGNAL_LOGOFF;
        break;

    case CTRL_SHUTDOWN_EVENT:
        event = OS_SIGNAL_SHUTDOWN;
        break;

    default:
        return FALSE;
    }


    /*
     * Only record the event and wake the worker.
     *
     * Don't call application code here.
     */
    InterlockedExchange(
        &g_signal_type,
        (LONG)event
    );

    SetEvent(g_signal_event);

    return TRUE;
}


static DWORD WINAPI os_signal_thread(LPVOID arg)
{
    tbox_signal_thread_t *thread = arg;

    while (1) {

        DWORD result;

        result = WaitForSingleObject(
            thread->event,
            INFINITE
        );

        if (result != WAIT_OBJECT_0)
            break;


        tbox_signal_t event =
            (tbox_signal_t)InterlockedExchange(
                &g_signal_type,
                OS_SIGNAL_NONE
            );


        if (event == OS_SIGNAL_NONE)
            break;


        if (g_callback != NULL)
            g_callback(event);


        /*
         * For this design, terminate the signal thread
         * after a termination event.
         */
        if (event == OS_SIGNAL_INTERRUPT ||
            event == OS_SIGNAL_BREAK ||
            event == OS_SIGNAL_TERMINATE ||
            event == OS_SIGNAL_LOGOFF ||
            event == OS_SIGNAL_SHUTDOWN) {

            break;
        }
    }

    return 0;
}


int tbox_os_signal_start(tbox_signal_thread_t *thread,
                    tbox_signal_callback_t callback)
{
    if (thread == NULL || callback == NULL)
        return -1;


    g_callback = callback;


    g_signal_event = CreateEventA(
        NULL,
        FALSE,      /* auto reset */
        FALSE,
        NULL
    );

    if (g_signal_event == NULL) {
        g_callback = NULL;
        return -1;
    }


    thread->event = g_signal_event;


    if (!SetConsoleCtrlHandler(
            os_console_handler,
            TRUE)) {

        CloseHandle(g_signal_event);

        g_signal_event = NULL;
        thread->event = NULL;
        g_callback = NULL;

        return -1;
    }


    thread->thread = CreateThread(
        NULL,
        0,
        os_signal_thread,
        thread,
        0,
        NULL
    );

    if (thread->thread == NULL) {

        SetConsoleCtrlHandler(
            os_console_handler,
            FALSE
        );

        CloseHandle(g_signal_event);

        g_signal_event = NULL;
        thread->event = NULL;
        g_callback = NULL;

        return -1;
    }


    return 0;
}


int tbox_os_signal_stop(tbox_signal_thread_t *thread)
{
    if (thread == NULL)
        return -1;


    /*
     * Remove the console handler first so that
     * new console events are no longer accepted.
     */
    SetConsoleCtrlHandler(
        os_console_handler,
        FALSE
    );


    /*
     * Wake the thread if it is waiting.
     */
    SetEvent(thread->event);


    WaitForSingleObject(
        thread->thread,
        INFINITE
    );


    CloseHandle(thread->thread);
    CloseHandle(thread->event);


    thread->thread = NULL;
    thread->event = NULL;

    g_signal_event = NULL;
    g_callback = NULL;

    return 0;
}