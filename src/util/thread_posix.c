/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * thread_posix.c - POSIX backend of the thread wrapper (Linux/macOS).
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L   /* pthread/nanosleep visible under -std=c17 */
#endif

#include "tbox_thread.h"
#include <stddef.h>
#include <time.h>


void tbox_sleep_ms(unsigned int ms)
{
    struct timespec ts;

    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000L);

    while (nanosleep(&ts, &ts) == -1)
        { /* EINTR: resume with the remaining time in ts */ }
}


int tbox_thread_start(tbox_thread_t *thread,
                      tbox_thread_fn fn,
                      void *arg)
{
    int ret;

    if (thread == NULL || fn == NULL)
        return -1;

    ret = pthread_create(&thread->thread, NULL, fn, arg);

    if (ret != 0)
        return -1;

    thread->started = 1;

    return 0;
}


int tbox_thread_join(tbox_thread_t *thread)
{
    int ret;

    if (thread == NULL || !thread->started)
        return -1;

    ret = pthread_join(thread->thread, NULL);

    if (ret != 0)
        return -1;

    thread->started = 0;

    return 0;
}


int tbox_mutex_init(tbox_mutex_t *mutex)
{
    pthread_mutexattr_t attr;
    int ret;

    if (mutex == NULL || mutex->initialized)
        return -1;

    /* Recursive, matching the Windows backend: a thread holding the archive
     * lock may call a listing helper that takes it again. */
    if (pthread_mutexattr_init(&attr) != 0)
        return -1;

    (void)pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);

    ret = pthread_mutex_init(&mutex->mutex, &attr);

    pthread_mutexattr_destroy(&attr);

    if (ret != 0)
        return -1;

    mutex->initialized = 1;

    return 0;
}


void tbox_mutex_free(tbox_mutex_t *mutex)
{
    if (mutex == NULL || !mutex->initialized)
        return;

    pthread_mutex_destroy(&mutex->mutex);
    mutex->initialized = 0;
}


void tbox_mutex_lock(tbox_mutex_t *mutex)
{
    if (mutex == NULL || !mutex->initialized)
        return;

    pthread_mutex_lock(&mutex->mutex);
}


void tbox_mutex_unlock(tbox_mutex_t *mutex)
{
    if (mutex == NULL || !mutex->initialized)
        return;

    pthread_mutex_unlock(&mutex->mutex);
}
