/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * signal_posix.c - POSIX backend of the interrupt listener (Linux/macOS).
 *
 * Classic self-pipe trick: a signal handler must only call async-signal-safe
 * functions, so SIGINT/SIGTERM/SIGHUP write ONE byte into a pipe. A listener
 * thread blocks on read() of that pipe and turns "a byte arrived" into the
 * same sticky flags the Windows backend produces. Every other signal in the
 * process stays untouched; the bridge worker never blocked on read() here -
 * it only polls tbox_signal_pending() between td_receive calls (threading
 * law).
 *
 * See tbox_signal.h for the cross-platform contract. CMake selects this file on
 * non-WIN32 and links pthreads.
 */

#if defined(_WIN32)
#error "signal_posix.c is the POSIX backend - CMake selects it only on non-WIN32"
#endif

/* Strict -std=c17 hides POSIX functions behind glibc feature-test macros;
 * opt in so sigaction/pipe/poll/pthread are visible. Must precede ALL the
 * system headers below. */
#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 200809L
#define _POSIX_C_SOURCE 200809L
#endif

#include <signal.h>
#include <stdio.h>        /* fputs */
#include <stdlib.h>       /* NULL */
#include <string.h>       /* memset, sigaction mask setup */
#include <unistd.h>       /* pipe, read, write, close */
#include <poll.h>         /* poll */
#include <pthread.h>
#include <stdatomic.h>

#include "tbox_signal.h"

/* ---- shared state ------------------------------------------------------- */
static atomic_int g_interrupted; /* sticky: any interrupt received           */
static atomic_int g_force;       /* sticky: second interrupt -> force        */
static atomic_int g_notified;    /* sticky: one-line notice already shown    */
static atomic_int g_run;         /* 1 = listener alive                       */
static int        g_pipefd[2] = { -1, -1 };  /* self-pipe                   */
static pthread_t  g_thread;
static int        g_have_thread;
static int        g_ready;       /* install/shutdown guard (startup-only)    */

/* ---- async-signal-safe half ---------------------------------------------- */
static void handle_signal(int sig)
{
    /* Only write() - everything else is done on the listener thread. */
    (void)sig;
    unsigned char b = 1;
    ssize_t n = write(g_pipefd[1], &b, 1);
    (void)n;
}

static void install_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sa.sa_flags = SA_RESTART;            /* don't interrupt blocking read()  */
    sigemptyset(&sa.sa_mask);

    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
}

static void restore_handlers(void)
{
    signal(SIGINT,  SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP,  SIG_DFL);
}

/* ---- listener thread ------------------------------------------------------- *
 * Blocks on the pipe; a byte means "something happened". Flags are owned by
 * fire() (synchronous), this thread just prints the notice once and is the
 * Step 17 hook for waking a condvar. */
static void *listener_main(void *unused)
{
    unsigned char b;

    (void)unused;
    for (;;) {
        if (read(g_pipefd[0], &b, 1) != 1)
            break;                          /* pipe closed: shutting down      */

        if (!atomic_load(&g_run))
            break;                          /* shutdown requested              */

        if (!atomic_exchange(&g_notified, 1))
            fputs("tbox: interrupt received - stopping (Ctrl+C again to force)\n",
                  stderr);
    }
    return NULL;
}

/* ---- public API ---------------------------------------------------------- */

void tbox_signal_install(void)
{
    if (g_ready)
        return;                             /* idempotent                      */

    if (pipe(g_pipefd) != 0) {              /* no listener: degrade            */
        g_pipefd[0] = g_pipefd[1] = -1;
        return;
    }

    atomic_store(&g_run, 1);
    install_handlers();
    g_ready = 1;

    if (pthread_create(&g_thread, NULL, listener_main, NULL) == 0)
        g_have_thread = 1;                  /* else handler still works: pipe
                                             * gets the byte, pending() works */
}

void tbox_signal_shutdown(void)
{
    unsigned char b = 1;

    if (!g_ready)
        return;

    restore_handlers();                     /* no new interrupts               */
    atomic_store(&g_run, 0);
    if (write(g_pipefd[1], &b, 1) == 1) {   /* wake the reader so it can exit  */
        if (g_have_thread) {
            pthread_join(g_thread, NULL);   /* it breaks after read + !g_run   */
            g_have_thread = 0;
        }
    }

    close(g_pipefd[0]);
    close(g_pipefd[1]);
    g_pipefd[0] = g_pipefd[1] = -1;

    atomic_store(&g_interrupted, 0);        /* clean slate for re-install      */
    atomic_store(&g_force, 0);
    atomic_store(&g_notified, 0);
    g_ready = 0;
}

int tbox_signal_pending(void)
{
    return atomic_load(&g_interrupted);
}

int tbox_signal_force(void)
{
    return atomic_load(&g_force);
}

int tbox_signal_wait(unsigned ms)
{
    struct pollfd pfd;
    int timeout;
    int rc;

    if (atomic_load(&g_interrupted))
        return 1;                           /* sticky: already interrupted     */

    if (g_pipefd[0] < 0)
        return 0;

    pfd.fd = g_pipefd[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    timeout = (int)(ms > 2000000000U ? 2000000000U : ms);   /* poll: int ms  */

    rc = poll(&pfd, 1, timeout);
    if (rc > 0)
        return 1;                           /* a byte arrived = an interrupt   */
    return atomic_load(&g_interrupted);     /* 0 on timeout / error            */
}

void tbox_signal_fire(void)
{
    const int was = atomic_exchange(&g_interrupted, 1);
    unsigned char b = 1;

    atomic_store(&g_force, was ? 1 : 0);    /* 2nd interrupt -> force          */
    if (g_pipefd[1] >= 0) {
        ssize_t n = write(g_pipefd[1], &b, 1);   /* wake the listener       */
        (void)n;
    }
}