/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#include <stdio.h>

#include "cli/cli.h"
#include "cmd/cmd.h"
#include "util/tbox_signal.h"

static void *worker(void *arg) 
{ /* Do work */ 
    (void)arg;   /* placeholder worker; no args used yet */
    return NULL; 
}

static void event_callback(tbox_os_event_t event)
{
    switch (event) {

        case OS_EVENT_INTERRUPT:
            printf("Ctrl+C received\n");
            break;

        case OS_EVENT_BREAK:
            printf("Break received\n");
            break;

        case OS_EVENT_TERMINATE:
            printf("Terminate received\n");
            break;

        default:
            break;
    }
}

int main(int argc, char *argv[])
{
    tbox_os_thread_t thread;

    tbox_os_event_start(event_callback);

    int created = tbox_os_thread_create(&thread, worker, NULL);


    int ret = tbox_cli(argc, argv);

    if (created == 0) {
        tbox_os_thread_join(&thread);
        tbox_os_thread_close(&thread);
    }

    tbox_os_event_stop();

    return ret;

}