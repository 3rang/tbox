/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TBOX_SIGNAL_H
#define TBOX_SIGNAL_H


#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(_WIN64)

#define OS_PLATFORM_WINDOWS
#include <windows.h>

#elif defined(__unix__) || defined(__APPLE__)

#define OS_PLATFORM_POSIX
#include <pthread.h>

#else

#error "Unsupported platform"

#endif

typedef struct {
   #if defined(OS_PLATFORM_WINDOWS)
    HANDLE handle;
    #elif defined(OS_PLATFORM_POSIX)
    pthread_t handle;
    #endif
} tbox_os_thread_t;

typedef void *(*tbox_os_thread_func_t)(void *arg);


/*
 * ============================================================
 * OS signal / console events
 * ============================================================
 */

/*
 * Common event types exposed to the application.
 *
 * Different OS-specific signals are converted into these
 * common events.
 */
typedef enum
{
    OS_EVENT_NONE = 0,

    /* User pressed Ctrl+C / SIGINT */
    OS_EVENT_INTERRUPT,

    /* Ctrl+Break / SIGQUIT */
    OS_EVENT_BREAK,

    /* SIGTERM / Windows console close */
    OS_EVENT_TERMINATE,

    /* SIGHUP / console/session related event */
    OS_EVENT_HANGUP,

    /* Windows logoff */
    OS_EVENT_LOGOFF,

    /* Windows shutdown */
    OS_EVENT_SHUTDOWN

} tbox_os_event_t;


/*
 * Callback called when an OS event occurs.
 */
typedef void (*tbox_os_event_func_t)(tbox_os_event_t event);


/*
 * ============================================================
 * Thread API
 * ============================================================
 */

/*
 * Start a thread.
 *
 * Returns:
 *   0  = success
 *  -1  = error
 */
int tbox_os_thread_create(tbox_os_thread_t *thread,
                    tbox_os_thread_func_t func,
                    void *arg);


/*
 * Wait for thread to finish.
 *
 * Returns:
 *   0  = success
 *  -1  = error
 */
int tbox_os_thread_join(tbox_os_thread_t *thread);


/*
 * Release thread resources.
 */
void tbox_os_thread_close(tbox_os_thread_t *thread);


/*
 * ============================================================
 * OS event API
 * ============================================================
 */

/*
 * Start OS event handling.
 *
 * The callback is called when Ctrl+C, SIGTERM, etc.
 * are received.
 */
int tbox_os_event_start(tbox_os_event_func_t callback);


/*
 * Stop OS event handling.
 */
void tbox_os_event_stop(void);


/*
 * Check whether an interrupt/termination event was received.
 *
 * Returns:
 *   1 = event received
 *   0 = no event
 */
int tbox_os_event_received(void);


#ifdef __cplusplus
}
#endif

#endif /* TBOX_SIGNAL_H */