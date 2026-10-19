/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * main.c — the tbox CLI entry point.
 *
 * Subcommands grow one chunk at a time:
 *   tbox version
 *   tbox auth-state <TDLib authorizationState name>   (debug: sees the table)
 *   tbox td-smoke                           load tdjson + sync call
 *   tbox login / whoami / logout                      (upcoming)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "td/tbox_td.h"
#include "td/tbox_td_auth.h"

#define TBOX_VERSION "0.1.0-dev"

static const char *STATE_NAMES[] = {
    "START", "WAIT_PHONE", "WAIT_QR", "WAIT_PASSWORD",
    "READY", "CLOSING", "CLOSED", "FAILED",
};
static const char *ACTION_NAMES[] = {
    "NONE", "REQUEST_QR", "SHOW_QR", "ASK_PASSWORD",
    "AUTHORIZED", "CLOSED", "FATAL",
};

static void print_usage(void) {
    printf("tbox — personal archive over Telegram Saved Messages (%s)\n\n"
           "  tbox version\n"
           "  tbox auth-state <authorizationState name>   show FSM step\n"
           "  tbox td-smoke <tdjson path>                 load lib + sync call\n"
           "  (login, whoami, logout are coming)\n",
           TBOX_VERSION);
}

static int cmd_version(void) {
    printf("tbox %s\n", TBOX_VERSION);
    return 0;
}

static int cmd_auth_state(const char *td_state) {
    TBoxAuthTransition t = tbox_auth_next(TBOX_AUTH_START, td_state);
    if (t.next == TBOX_AUTH_FAILED) {
        fprintf(stderr, "error: unknown authorizationState '%s'\n", td_state);
        return 1;
    }
    printf("%-42s -> state %-14s action %s\n",
           td_state, STATE_NAMES[t.next], ACTION_NAMES[t.action]);
    return 0;
}

static int cmd_td_smoke(void) {
    tbox_td_init(1);
    char *out = tbox_td_execute("{\"@type\":\"getLogStream\"}");
    int rc = 0;
    if (out != NULL) {
        printf("getLogStream -> %s\n", out);
        free(out);
    } else {
        fprintf(stderr, "error: td_execute gave no reply\n");
        rc = 1;
    }
    tbox_td_close();
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 2;
    }
    if (strcmp(argv[1], "version") == 0) {
        return cmd_version();
    }
    if (strcmp(argv[1], "auth-state") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: tbox auth-state <authorizationState name>\n");
            return 2;
        }
        return cmd_auth_state(argv[2]);
    }
    if (strcmp(argv[1], "td-smoke") == 0) {
        return cmd_td_smoke();
    }
    print_usage();
    return 2;
}
