/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#include <stdio.h>
#include <string.h>
#include "cli.h"
#include "cmd/cmd.h"

int tbox_cli(int argc, char *argv[])
{
    /* No command at all -> usage error: show help, exit 2. */
    if (argc < 2)
    {
        tbox_cmd_help();
        return TBOX_UNKNOWN_COMMAND;
    }

    if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)
    {
        tbox_cmd_help();
        return TBOX_EXIT_OK;
    }
    else if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)
    {
        tbox_cmd_version();
        return TBOX_EXIT_OK;
    }
    else if (strcmp(argv[1], "help") == 0)
    {
        tbox_cmd_help();
        return TBOX_EXIT_OK;
    }
    else if (strcmp(argv[1], "auth") == 0)
    {
        return tbox_cmd_auth(argc, argv);
    }
    else if (strcmp(argv[1], "serve") == 0)
    {
        return tbox_cmd_serve(argc, argv);
    }
    else if (strcmp(argv[1], "status") == 0)
    {
        return tbox_cmd_status(argc, argv);
    }
    else if (strcmp(argv[1], "selftest") == 0)
    {
        return tbox_cmd_selftest(argc, argv);
    }
    else
    {
        fprintf(stderr, "tbox: unknown command '%s'\n", argv[1]);
        tbox_cmd_help();
        return TBOX_UNKNOWN_COMMAND;
    }
}