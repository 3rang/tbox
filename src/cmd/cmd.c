/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#include <stdio.h>
#include <string.h>
#include "cmd.h"

#ifndef TBOX_VERSION
#define TBOX_VERSION "0.5.0"
#endif

void tbox_cmd_version(void)
{
    printf("tbox %s\n", TBOX_VERSION);
}

void tbox_cmd_help(void)
{
    printf("tbox - terminal Telegram box (C17 + TDLib)\n");
    printf("\n");
    printf("usage: tbox <command> [args]\n");
    printf("\n");
    printf("commands:\n");
    printf("  auth       log in via QR code (QR-only)\n");
    printf("  status     show session/account state\n");
    printf("  help       show this help\n");
    printf("  selftest   internal self-test (not exposed on CLI)\n");
    printf("\n");
    printf("flags:\n");
    printf("  -v, --version   print version\n");
    printf("  -h, --help      show this help\n");
    printf("\n");
    printf("unknown commands exit with code %d.\n", TBOX_UNKNOWN_COMMAND);
}