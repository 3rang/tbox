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