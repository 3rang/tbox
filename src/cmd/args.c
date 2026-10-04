/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * cmd/args.c - the argument handling every command shares.
 *
 * auth, serve and status all take the same optional `--data <dir>`, and all
 * three must agree on where the session lives, so the parsing and the root
 * resolution live here once instead of being copied per command.
 */

#include <stdio.h>
#include <string.h>

#include "cmd.h"
#include "util/datadir.h"

/*
 * usage: <cmd> [--data <dir>]
 *
 * `data_dir` comes back as NULL when the flag was absent, which every caller
 * reads as "use the per-OS default". Returns TBOX_EXIT_OK, or
 * TBOX_UNKNOWN_COMMAND (bad usage) with a message already printed.
 */
int tbox_cmd_parse_data(int argc, char *argv[], const char **data_dir)
{
    int i;

    *data_dir = NULL;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s: --data needs a directory\n", argv[1]);
                return TBOX_UNKNOWN_COMMAND;
            }
            *data_dir = argv[++i];
        } else {
            fprintf(stderr, "%s: unexpected argument '%s'\n", argv[1], argv[i]);
            return TBOX_UNKNOWN_COMMAND;
        }
    }

    return TBOX_EXIT_OK;
}

/*
 * Resolve the data root (`--data`, else the per-OS default) into `out`.
 * `cmd` is the command name to blame in error messages.
 *
 * create = 1: make sure the directory exists (serve owns a directory).
 * create = 0: only resolve the path - `status` is strictly read-only, so
 *             asking about a session that was never created changes nothing.
 *
 * Returns 0, or -1 with a message already printed.
 */
int tbox_cmd_resolve_root(const char *cmd, const char *data_dir, char *out,
                          size_t size, int create)
{
    if (data_dir != NULL) {
        size_t len = strlen(data_dir);

        if (len == 0 || len >= size) {
            fprintf(stderr, "%s: --data path is empty or too long.\n", cmd);
            return -1;
        }
        memcpy(out, data_dir, len + 1);
    } else if (tbox_datadir_default(out, size) != 0) {
        fprintf(stderr, "%s: could not locate a data directory "
                        "(no home directory?).\n", cmd);
        return -1;
    }

    if (create && tbox_datadir_ensure(out) != 0) {
        fprintf(stderr, "%s: could not create %s\n", cmd, out);
        return -1;
    }

    return 0;
}