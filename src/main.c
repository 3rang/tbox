/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#include <stdio.h>
#include <stdbool.h>

#include "cli/cli.h"
#include "cmd/cmd.h"
#include "util/tbox_signal.h"
#ifdef TBOX_WITH_TDLIB
#include "td/tdhandler.h"
#include "td/tdserve.h"
#endif

/*
 * Shared termination flag (single source of truth).
 *
 * The signal-listener thread sets it true when the OS delivers
 * Ctrl+C / Break / Close / Logoff / Shutdown. cmd/cmd.c passes it to
 * the auth worker, which polls it on every td_receive tick and closes
 * TDLib cooperatively - threads are never killed mid-call.
 */
static volatile bool app_stopping = false;

static void app_signal_handler(tbox_signal_t signal)
{
    switch (signal) {

    case OS_SIGNAL_INTERRUPT:
        printf("Ctrl+C received\n");
        app_stopping = true;
        break;

    case OS_SIGNAL_TERMINATE:
        printf("Terminate received\n");
        app_stopping = true;
        break;

    case OS_SIGNAL_BREAK:
        printf("Break received\n");
        app_stopping = true;
        break;

    case OS_SIGNAL_LOGOFF:
        printf("Logoff received\n");
        app_stopping = true;
        break;

    case OS_SIGNAL_SHUTDOWN:
        printf("Shutdown received\n");
        app_stopping = true;
        break;

    default:
        break;
    }
}

int main(int argc, char *argv[])
{
    tbox_signal_thread_t signal_thread;

    int started = tbox_os_signal_start(&signal_thread, app_signal_handler);

#ifdef TBOX_WITH_TDLIB
    /* `auth` and `serve` run TDLib on their own worker threads; the signal
     * thread listens side by side and flips app_stopping to cancel them.
     * The td layer is linked into this binary, so the entry points are handed
     * to the cmd layer here (see cmd/cmd.h) and never linked from tboxcore. */
    tbox_cmd_set_tdlib_present(true);
    tbox_cmd_set_auth(tbox_td_auth_run, &app_stopping);

    /* `serve` gets the td layer's runner: the session, the Saved Messages
     * scan, the FTP server and the rescan loop (src/td/tdserve.c). */
    tbox_cmd_set_serve(tbox_td_serve_run, &app_stopping);
#endif

    int ret = tbox_cli(argc, argv);

    if (started == 0)
        tbox_os_signal_stop(&signal_thread);

    return ret;
}
