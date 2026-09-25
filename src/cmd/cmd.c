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

static const char *progname(const char *argv0);

/*
 * Command table. exposed=1 entries are selectable from argv (auth/status/
 * help). exposed=0 entries exist in the internal registry but are NOT
 * reachable from the command line - the CLI surface stays intentionally
 * small while the internals grow real commands.
 */
static cmd_entry table[] = {
    { "auth",     cmd_auth,     "log in via QR code (QR-only)",            1 },
    { "status",   cmd_status,   "show session/account state",              1 },
    { "help",     cmd_help,     "show this help",                          1 },
    { "selftest", cmd_selftest, "internal self-test (not exposed on CLI)", 0 },
};

int tbox_cmd_run(int argc, char *argv[])
{
    size_t i;

    if (argc < 2 || argv[1] == NULL)
        return TBOX_EXIT_USAGE;

    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (table[i].exposed && strcmp(argv[1], table[i].name) == 0)
            return table[i].fn(argc, argv);
    }

    fprintf(stderr, "tbox: unknown command '%s'\n", argv[1]);
    fprintf(stderr, "run 'tbox help' for usage\n");
    return TBOX_EXIT_USAGE;
}

cmd_fn tbox_cmd_by_name(const char *name)
{
    size_t i;

    if (name == NULL)
        return NULL;

    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(name, table[i].name) == 0)
            return table[i].fn;
    }
    return NULL;
}

/* ---- command bodies (stubs; real logic lands in later ladder steps) ------ */

int cmd_auth(int argc, char *argv[])
{
    (void)argc; (void)argv;
    fprintf(stderr, "auth: not implemented yet (terminal QR login)\n");
    return TBOX_EXIT_NOT_IMPL;
}

int cmd_status(int argc, char *argv[])
{
    (void)argc; (void)argv;
    fprintf(stderr, "status: not implemented yet\n");
    return TBOX_EXIT_NOT_IMPL;
}

int cmd_selftest(int argc, char *argv[])
{
    (void)argc; (void)argv;
    fprintf(stderr, "selftest: internal only - not visible on the CLI\n");
    return TBOX_EXIT_NOT_IMPL;
}

int cmd_version(int argc, char *argv[])
{
    printf("%s %s\n", progname(argc > 0 ? argv[0] : NULL), TBOX_VERSION);
    return TBOX_EXIT_OK;
}

int cmd_help(int argc, char *argv[])
{
    const char *prog = progname(argc > 0 ? argv[0] : NULL);
    size_t i;

    printf("%s - terminal Telegram box (C17 + TDLib)\n", prog);
    printf("\n");
    printf("usage: %s <command> [args]\n", prog);
    printf("\n");
    printf("commands:\n");
    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (table[i].exposed)
            printf("  %-9s %s\n", table[i].name, table[i].summary);
    }
    printf("\n");
    printf("flags:\n");
    printf("  -v, --version   print version\n");
    printf("  -h, --help      show this help\n");
    printf("\n");
    printf("unknown commands exit with code %d.\n", TBOX_EXIT_USAGE);
    return TBOX_EXIT_OK;
}

static const char *progname(const char *argv0)
{
    const char *bs = argv0 ? strrchr(argv0, '\\') : NULL;
    const char *fs = argv0 ? strrchr(argv0, '/') : NULL;
    const char *sep = (bs && fs) ? (bs > fs ? bs : fs) : (bs ? bs : fs);
    return sep ? sep + 1 : (argv0 ? argv0 : "tbox");
}