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


int tbox_os_thread_create(tbox_os_thread_t *thread,
                     tbox_os_thread_func_t func,
                     void *arg);

int tbox_os_thread_join(tbox_os_thread_t *thread);


void tbox_os_thread_close(tbox_os_thread_t *thread);

#ifdef __cplusplus
}
#endif

#endif /* TBOX_SIGNAL_H */