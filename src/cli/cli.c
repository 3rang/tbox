/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * cli.c — parse + dispatch. Command bodies live in cmd/; this file only
 * knows the command names, their argument arity, and the usage text.
 */
#include "cli/cli.h"

#include <stdio.h>
#include <string.h>

#include "cmd/cmd.h"

static void print_usage(void) {
    printf("tbox - personal archive over Telegram Saved Messages (%s)\n\n"
           "  tbox version\n"
           "  tbox auth-state <authorizationState name>   show FSM step\n"
           "  tbox td-smoke                               load tdjson + sync call\n"
           "  tbox login <session-dir> <api-id> <api-hash>   drive auth (json events)\n"
           "  (whoami, logout are coming)\n",
           TBOX_VERSION);
}

int tbox_cli(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage();
        return TBOX_UNKNOWN_COMMAND;
    }
    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_usage();
        return TBOX_EXIT_OK;
    }
    if (strcmp(argv[1], "version") == 0) {
        return tbox_cmd_version();
    }
    if (strcmp(argv[1], "auth-state") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: tbox auth-state <authorizationState name>\n");
            return TBOX_UNKNOWN_COMMAND;
        }
        return tbox_cmd_auth_state(argv[2]);
    }
    if (strcmp(argv[1], "td-smoke") == 0) {
        return tbox_cmd_td_smoke();
    }
    if (strcmp(argv[1], "login") == 0) {
        if (argc < 5) {
            fprintf(stderr,
                    "usage: tbox login <session-dir> <api-id> <api-hash>\n");
            return TBOX_UNKNOWN_COMMAND;
        }
        return tbox_cmd_login(argv[2], argv[3], argv[4]);
    }
    print_usage();
    return TBOX_UNKNOWN_COMMAND;
}
