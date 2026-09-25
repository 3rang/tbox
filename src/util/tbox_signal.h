/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TBOX_SIGNAL_H
#define TBOX_SIGNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Platform selection
 */
#if defined(_WIN32) || defined(_WIN64)

    #define OS_SIGNAL_WINDOWS
    #include <windows.h>

#elif defined(__linux__) || defined(__APPLE__)

    #define OS_SIGNAL_POSIX
    #include <pthread.h>
    #include <signal.h>

#else

    #error "Unsupported operating system"

#endif


/*
 * Common signals/events exposed to the application.
 */
typedef enum
{
    OS_SIGNAL_NONE = 0,

    /* Ctrl+C / SIGINT */
    OS_SIGNAL_INTERRUPT,

    /* Ctrl+Break / SIGQUIT */
    OS_SIGNAL_BREAK,

    /* SIGTERM / console close */
    OS_SIGNAL_TERMINATE,

    /* SIGHUP */
    OS_SIGNAL_HANGUP,

    /* Windows logoff */
    OS_SIGNAL_LOGOFF,

    /* Windows shutdown */
    OS_SIGNAL_SHUTDOWN

} tbox_signal_t;


/*
 * Application callback.
 *
 * This callback is executed by the signal thread,
 * NOT directly from the OS signal handler.
 */
typedef void (*tbox_signal_callback_t)(tbox_signal_t signal);


/*
 * Signal thread handle.
 */
typedef struct
{
#if defined(OS_SIGNAL_WINDOWS)

    HANDLE thread;
    HANDLE event;

#elif defined(OS_SIGNAL_POSIX)

    pthread_t thread;
    sigset_t set;

#endif

} tbox_signal_thread_t;


/*
 * Start signal handling thread.
 *
 * The signal thread waits for OS signals/events while
 * the main application continues running.
 *
 * Returns:
 *   0  = success
 *  -1  = failure
 */
int tbox_os_signal_start(tbox_signal_thread_t *thread,
                    tbox_signal_callback_t callback);


/*
 * Stop signal handling thread and release resources.
 *
 * Returns:
 *   0  = success
 *  -1  = failure
 */
int tbox_os_signal_stop(tbox_signal_thread_t *thread);


#ifdef __cplusplus
}
#endif

#endif /* TBOX_SIGNAL_H */