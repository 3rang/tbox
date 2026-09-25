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
   if (argc < 2 || argv[1] == NULL)
   {
       tbox_cmd_help();
       return TBOX_ERROR;
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
    else
    {
        tbox_cmd_help();
        return TBOX_UNKNOWN_COMMAND;
    }
}