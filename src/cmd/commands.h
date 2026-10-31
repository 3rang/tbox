/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * commands.h - one function per sub-command. argv[0] is the first
 * argument after the command name. Each returns an exit code.
 */
#ifndef TBOX_COMMANDS_H
#define TBOX_COMMANDS_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

int cmd_login(int argc, char **argv);
int cmd_logout(int argc, char **argv);
int cmd_send(int argc, char **argv);
int cmd_upload(int argc, char **argv);
int cmd_watch(int argc, char **argv);

/* argv[from..] joined with single spaces, so quoting the text is optional. */
static inline void join_args(int argc, char **argv, int from, char *buf, size_t n) {
    buf[0] = '\0';
    for (int i = from; i < argc; i++) {
        size_t used = strlen(buf);
        snprintf(buf + used, n - used, "%s%s", i > from ? " " : "", argv[i]);
    }
}

#endif
