/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#include <stdio.h>
#include "cmd.h"

#ifndef TBOX_VERSION
#define TBOX_VERSION "0.5.0"
#endif

void tbox_cmd_version(void)
{
    printf("tbox %s\n", TBOX_VERSION);
}

/* ---- command stubs (real logic lands with the login work) ---------------- */

int tbox_cmd_auth(int argc, char *argv[])
{
    (void)argc; (void)argv;
    fprintf(stderr, "auth: not implemented yet (terminal QR login)\n");
   // while (1) {} /* placeholder to avoid unused variable warning */
    return TBOX_ERROR;
}

int tbox_cmd_status(int argc, char *argv[])
{
    (void)argc; (void)argv;
    fprintf(stderr, "status: not implemented yet\n");
    return TBOX_ERROR;
}

int tbox_cmd_selftest(int argc, char *argv[])
{
    (void)argc; (void)argv;
    fprintf(stderr, "selftest: not implemented yet\n");
    return TBOX_ERROR;
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
    printf("  selftest   run the internal self-test\n");
    printf("\n");
    printf("flags:\n");
    printf("  -v, --version   print version\n");
    printf("  -h, --help      show this help\n");
    printf("\n");
    printf("unknown commands exit with code %d.\n", TBOX_UNKNOWN_COMMAND);
}