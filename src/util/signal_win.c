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

typedef struct { 
    tbox_os_thread_func_t func; 
    void *arg; 
}os_thread_context_t; 

static DWORD WINAPI os_thread_entry(LPVOID arg) { 
    os_thread_context_t *ctx = arg; 
    ctx->func(ctx->arg); 
    HeapFree(GetProcessHeap(), 0, ctx); 
    return 0; 
} 


int tbox_os_thread_create(tbox_os_thread_t *thread,
                     tbox_os_thread_func_t func,
                     void *arg)
{
    if (!thread || !func) {
        return -1;
    }

    os_thread_context_t *ctx = HeapAlloc(GetProcessHeap(), 0, sizeof(os_thread_context_t));
    if (!ctx) {
        return -1;
    }
    ctx->func = func;
    ctx->arg = arg;

    thread->handle = CreateThread(NULL, 0, os_thread_entry, ctx, 0, NULL);

    if(!thread->handle) {
        free(ctx);
        return -1;
    }

    return 0;
}

int tbox_os_thread_join(tbox_os_thread_t *thread)
{
   DWORD result; 
   result = WaitForSingleObject( thread->handle, INFINITE ); 
   return (result == WAIT_OBJECT_0) ? 0 : -1;
}

void tbox_os_thread_close(tbox_os_thread_t *thread)
{
    if (thread->handle != NULL) { 
        CloseHandle(thread->handle); 
        thread->handle = NULL; 
    }
}