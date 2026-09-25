/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#include "cli/cli.h"
#include "cmd/cmd.h"
#include "util/tbox_signal.h"

static void *worker(void *arg) 
{ /* Do work */ 
    (void)arg;   /* placeholder worker; no args used yet */
    return NULL; 
}


int main(int argc, char *argv[])
{
    tbox_os_thread_t thread;
    int created = tbox_os_thread_create(&thread, worker, NULL);

    int ret = tbox_cli(argc, argv);

    if (created == 0) {
        tbox_os_thread_join(&thread);
        tbox_os_thread_close(&thread);
    }

    return ret;

}