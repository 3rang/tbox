/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * signal_posix.c - POSIX backend of the interrupt listener (Linux/macOS).
 */

#include "tbox_signal.h"
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

/*
 * Application callback.
 */
static tbox_os_event_func_t g_callback = NULL;

/*
 * Set by the signal handler.
 *
 * sig_atomic_t is specifically intended for communication
 * between a signal handler and normal code.
 */
static volatile sig_atomic_t g_event = OS_EVENT_NONE;


/*
 * Convert POSIX signal -> common OS event.
 */
static void os_signal_handler(int signal)
{
    switch (signal) {

        case SIGINT:
            g_event = OS_EVENT_INTERRUPT;
            break;

#ifdef SIGQUIT
        case SIGQUIT:
            g_event = OS_EVENT_BREAK;
            break;
#endif

        case SIGTERM:
            g_event = OS_EVENT_TERMINATE;
            break;

#ifdef SIGHUP
        case SIGHUP:
            g_event = OS_EVENT_HANGUP;
            break;
#endif

        default:
            break;
    }
}


/*
 * Install POSIX signal handlers.
 */
int tbox_os_event_start(tbox_os_event_func_t callback)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = os_signal_handler;

    sigemptyset(&sa.sa_mask);

    g_callback = callback;
    g_event = OS_EVENT_NONE;

    if (sigaction(SIGINT, &sa, NULL) != 0)
        return -1;

    if (sigaction(SIGTERM, &sa, NULL) != 0)
        return -1;

#ifdef SIGQUIT
    if (sigaction(SIGQUIT, &sa, NULL) != 0)
        return -1;
#endif

#ifdef SIGHUP
    if (sigaction(SIGHUP, &sa, NULL) != 0)
        return -1;
#endif

    return 0;
}


void tbox_os_event_stop(void)
{
    g_callback = NULL;
}


/*
 * Application can periodically call this.
 */
int tbox_os_event_received(void)
{
    tbox_os_event_t event;

    event = g_event;

    if (event == OS_EVENT_NONE)
        return 0;

    g_event = OS_EVENT_NONE;

    if (g_callback != NULL)
        g_callback(event);

    return 1;
}

/* ---- thread API ----------------------------------------------------------- */

int tbox_os_thread_create(tbox_os_thread_t *thread,
                     tbox_os_thread_func_t func,
                     void *arg)
{
    if (!thread || !func) {
        return -1;
    }

    return pthread_create(&thread->handle, NULL, func, arg);
}

int tbox_os_thread_join(tbox_os_thread_t *thread)
{
    if (!thread) {
        return -1;
    }

    return pthread_join(thread->handle, NULL);
}

void tbox_os_thread_close(tbox_os_thread_t *thread)
{
    if (!thread) {
        return;
    }

    pthread_detach(thread->handle);
}