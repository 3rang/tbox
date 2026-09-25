/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#include "cli/cli.h"
#include "cmd/cmd.h"
#include "util/tbox_signal.h"

int main(int argc, char *argv[])
{
    /* The interrupt listener must be up before any activity starts: a Ctrl+C
     * during startup is a real stop request, not something to lose. */
    tbox_signal_install();

    int rc = tbox_cli(argc, argv);

    /* Interrupted while the (possibly long-running) command ran -> tell the
     * shell. 130 = interrupted, per the exit-code table. */
    if (rc == TBOX_EXIT_OK && tbox_signal_pending())
        rc = TBOX_EXIT_INTR;

    tbox_signal_shutdown();
    return rc;
}