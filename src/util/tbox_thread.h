/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TBOX_THREAD_H
#define TBOX_THREAD_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Platform selection (same layout as util/tbox_signal.h).
 */
#if defined(_WIN32) || defined(_WIN64)

    #define OS_THREAD_WINDOWS
    #include <windows.h>

#elif defined(__linux__) || defined(__APPLE__)

    #define OS_THREAD_POSIX
    #include <pthread.h>

#else

    #error "Unsupported operating system"

#endif


/*
 * Thread entry point. Runs on the new thread; the return value is
 * ignored. Share results through the argument struct (it must stay
 * alive until tbox_thread_join() returns).
 */
typedef void *(*tbox_thread_fn)(void *arg);


/*
 * Thread handle.
 */
typedef struct
{
#if defined(OS_THREAD_WINDOWS)

    HANDLE thread;

#elif defined(OS_THREAD_POSIX)

    pthread_t thread;

#endif

    int started;   /* 1 while a joinable thread is running */

} tbox_thread_t;


/*
 * Start a worker thread.
 *
 * Returns:
 *   0  = success
 *  -1  = failure
 */
int tbox_thread_start(tbox_thread_t *thread,
                      tbox_thread_fn fn,
                      void *arg);


/*
 * Wait for the worker to finish and release its resources.
 * Blocks until the entry function returns.
 *
 * Returns:
 *   0  = success
 *  -1  = failure (NULL handle / never started)
 */
int tbox_thread_join(tbox_thread_t *thread);


/*
 * Sleep the calling thread (portable OS primitive).
 */
void tbox_sleep_ms(unsigned int ms);


/*
 * A plain mutex, from the same backends as the thread wrapper: one handle,
 * one native object per OS (CRITICAL_SECTION / pthread_mutex_t).
 *
 * Not recursive. Used for the two places where threads genuinely share
 * mutable state: the archive index that FTP sessions read while a rescan may
 * be rebuilding it, and the TDLib request/response table.
 */
typedef struct
{
#if defined(OS_THREAD_WINDOWS)

    CRITICAL_SECTION cs;

#elif defined(OS_THREAD_POSIX)

    pthread_mutex_t mutex;

#endif

    int initialized;   /* 1 once tbox_mutex_init() succeeded */

} tbox_mutex_t;


/* Initialise a mutex. Returns 0, or -1 on failure. */
int tbox_mutex_init(tbox_mutex_t *mutex);


/* Release a mutex. Safe on a zero-initialized (never used) handle. */
void tbox_mutex_free(tbox_mutex_t *mutex);


void tbox_mutex_lock(tbox_mutex_t *mutex);
void tbox_mutex_unlock(tbox_mutex_t *mutex);


#ifdef __cplusplus
}
#endif

#endif /* TBOX_THREAD_H */
