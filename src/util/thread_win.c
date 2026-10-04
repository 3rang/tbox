/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * thread_win.c - Windows backend of the thread wrapper.
 */

#include "tbox_thread.h"
#include <stdlib.h>


/*
 * CreateThread needs a WINAPI-typed routine, but callers pass a plain
 * cdecl fn(void*). The shim carries both across the boundary.
 */
struct thread_shim
{
    tbox_thread_fn fn;
    void *arg;
};


static DWORD WINAPI thread_trampoline(LPVOID param)
{
    struct thread_shim *shim = param;
    tbox_thread_fn fn = shim->fn;
    void *arg = shim->arg;

    free(shim);

    fn(arg);

    return 0;
}


int tbox_thread_start(tbox_thread_t *thread,
                      tbox_thread_fn fn,
                      void *arg)
{
    struct thread_shim *shim;

    if (thread == NULL || fn == NULL)
        return -1;

    shim = malloc(sizeof *shim);

    if (shim == NULL)
        return -1;

    shim->fn = fn;
    shim->arg = arg;

    thread->thread = CreateThread(
        NULL,
        0,
        thread_trampoline,
        shim,
        0,
        NULL
    );

    if (thread->thread == NULL) {
        free(shim);
        return -1;
    }

    thread->started = 1;

    return 0;
}


void tbox_sleep_ms(unsigned int ms)
{
    Sleep(ms);
}


int tbox_thread_join(tbox_thread_t *thread)
{
    if (thread == NULL || !thread->started)
        return -1;

    if (WaitForSingleObject(thread->thread, INFINITE) != WAIT_OBJECT_0)
        return -1;

    CloseHandle(thread->thread);

    thread->thread = NULL;
    thread->started = 0;

    return 0;
}


int tbox_mutex_init(tbox_mutex_t *mutex)
{
    if (mutex == NULL || mutex->initialized)
        return -1;

    /* The recursive flag is deliberate: a thread that must hold the archive
     * lock may itself call a listing helper that takes it again, and turning
     * that into a deadlock helps nobody. */
    if (!InitializeCriticalSectionAndSpinCount(&mutex->cs, 4000))
        return -1;

    mutex->initialized = 1;

    return 0;
}


void tbox_mutex_free(tbox_mutex_t *mutex)
{
    if (mutex == NULL || !mutex->initialized)
        return;

    DeleteCriticalSection(&mutex->cs);
    mutex->initialized = 0;
}


void tbox_mutex_lock(tbox_mutex_t *mutex)
{
    if (mutex == NULL || !mutex->initialized)
        return;

    EnterCriticalSection(&mutex->cs);
}


void tbox_mutex_unlock(tbox_mutex_t *mutex)
{
    if (mutex == NULL || !mutex->initialized)
        return;

    LeaveCriticalSection(&mutex->cs);
}
