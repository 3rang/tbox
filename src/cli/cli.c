/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#include <stdio.h>
#include <string.h>
#include "cli.h"
#include "cmd/cmd.h"

/*
 * Last path component of argv[0], so messages read "tbox" on every OS
 * (Windows backslash separator or POSIX forward slash).
 */
static const char *progname(const char *argv0)
{
    const char *bs = argv0 ? strrchr(argv0, '\\') : NULL;
    const char *fs = argv0 ? strrchr(argv0, '/') : NULL;
    const char *sep = (bs && fs) ? (bs > fs ? bs : fs) : (bs ? bs : fs);
    return sep ? sep + 1 : (argv0 ? argv0 : "tbox");
}

int tbox_cli(int argc, char *argv[])
{
    const char *prog = progname(argc > 0 ? argv[0] : NULL);

    /* No command at all -> short usage hint on stderr (exit 2). */
    if (argc < 2) {
        fprintf(stderr, "usage: %s <command> [args]   (run '%s help')\n",
                prog, prog);
        return TBOX_EXIT_USAGE;
    }

    /* Explicit help flags -> the help command. */
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
        return cmd_help(argc, argv);

    /* Version flags -> the version command. */
    if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0)
        return cmd_version(argc, argv);

    /* Everything else goes through the internal command table. */
    return tbox_cmd_run(argc, argv);
}