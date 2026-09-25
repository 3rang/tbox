/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#include <stdio.h>
#include <stdbool.h>

#include "cli/cli.h"
#include "cmd/cmd.h"
#include "util/tbox_signal.h"

static volatile bool app_running = true;

void *worker(void *arg) 
{ /* Do work */ 
    (void)arg;   /* placeholder worker; no args used yet */
    return NULL; 
}

static void app_signal_handler(tbox_signal_t signal)
{
    switch (signal) {

    case OS_SIGNAL_INTERRUPT:
        printf("Ctrl+C received\n");
        app_running = false;
        break;

    case OS_SIGNAL_TERMINATE:
        printf("Terminate received\n");
        app_running = false;
        break;

    case OS_SIGNAL_BREAK:
        printf("Break received\n");
        app_running = false;
        break;

    case OS_SIGNAL_LOGOFF:
        printf("Logoff received\n");
        app_running = false;
        break;

    case OS_SIGNAL_SHUTDOWN:
        printf("Shutdown received\n");
        app_running = false;
        break;

    default:
        break;
    }
}


int main(int argc, char *argv[])
{
    tbox_signal_thread_t signal_thread;

    int started = tbox_os_signal_start(&signal_thread, app_signal_handler);

    int ret = tbox_cli(argc, argv);

    if (started == 0)
        tbox_os_signal_stop(&signal_thread);

    return ret;

}