/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * signal_posix.c - POSIX backend of the interrupt listener (Linux/macOS).
 */

#include "tbox_signal.h"
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>


/*
 * Callback shared with signal thread.
 */
static tbox_signal_callback_t g_callback = NULL;


/*
 * Signal thread.
 *
 * Signals are synchronously received using sigwait().
 */
static void *tbox_os_signal_thread(void *arg)
{
    tbox_signal_thread_t *ctx = arg;
    int sig;
    int ret;

    while (1) {

        /*
         * Block here until one of the signals arrives.
         */
        ret = sigwait(&ctx->set,&sig);

        if (ret != 0) {
            fprintf(
                stderr,
                "sigwait() failed: %d\n",
                ret
            );

            break;
        }

        switch (sig) {

        case SIGINT:

            if (g_callback != NULL)
                g_callback(OS_SIGNAL_INTERRUPT);

            break;


        case SIGTERM:

            if (g_callback != NULL)
                g_callback(OS_SIGNAL_TERMINATE);

            break;


#ifdef SIGQUIT
        case SIGQUIT:

            if (g_callback != NULL)
                g_callback(OS_SIGNAL_BREAK);

            break;
#endif


#ifdef SIGHUP
        case SIGHUP:

            if (g_callback != NULL)
                g_callback(OS_SIGNAL_HANGUP);

            break;
#endif


        default:
            break;
        }
    }

    return NULL;
}


int tbox_os_signal_start(tbox_signal_thread_t *thread,
                    tbox_signal_callback_t callback)
{
    int ret;

    if (thread == NULL || callback == NULL)
        return -1;


    /*
     * Configure signals that the signal thread will receive.
     */
    sigemptyset(&thread->set);

    sigaddset(&thread->set, SIGINT);
    sigaddset(&thread->set, SIGTERM);

#ifdef SIGQUIT
    sigaddset(&thread->set, SIGQUIT);
#endif

#ifdef SIGHUP
    sigaddset(&thread->set, SIGHUP);
#endif


    /*
     * Block these signals in the calling thread.
     *
     * Newly created threads inherit this signal mask.
     */
    ret = pthread_sigmask(SIG_BLOCK,&thread->set,NULL);

    if (ret != 0) {

        fprintf(
            stderr,
            "pthread_sigmask() failed: %d\n",
            ret
        );

        return -1;
    }


    g_callback = callback;


    /*
     * Start dedicated signal thread.
     */
    ret = pthread_create(&thread->thread,NULL,tbox_os_signal_thread,thread);

    if (ret != 0) {

        fprintf(
            stderr,
            "pthread_create() failed: %d\n",
            ret
        );

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
     * Cancel the signal thread.
     *
     * sigwait() is a cancellation point on POSIX
     * implementations.
     */
    pthread_cancel(thread->thread);

    pthread_join(thread->thread,NULL);


    g_callback = NULL;


    /*
     * Note:
     *
     * The signal mask of the calling thread remains blocked.
     * If you want to restore the original mask, store the
     * previous mask during os_signal_start().
     */

    memset(thread,0,sizeof(*thread));

    return 0;
}