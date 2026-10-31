/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox - a small Telegram command line client in C.
 */
#include <stdio.h>
#include <string.h>

#include "cmd/commands.h"
#include "core/platform.h"
#include "core/session.h"

static const struct {
    const char *name;
    int (*run)(int argc, char **argv);
    const char *usage;
    const char *help;
} COMMANDS[] = {
    { "login", cmd_login, "login [--api-id ID --api-hash HASH]", "sign in (once); asks for phone, code, password" },
    { "logout", cmd_logout, "logout", "sign out and forget the session" },
    { "me", cmd_me, "me", "show the logged-in account" },
    { "search", cmd_search, "search <query>", "find chats by title or public @username" },
    { "send", cmd_send, "send <chat> <text...>", "send a text message" },
    { "read", cmd_read, "read <chat> [-n COUNT]", "print the last messages (default 20)" },
    { "upload", cmd_upload, "upload <chat> <file> [caption...]", "send a file" },
};

static void usage(FILE *out) {
    fprintf(out, "tbox %s - Telegram from the command line\n\n", TBOX_VERSION);
    fprintf(out, "usage: tbox <command> [args]\n\ncommands:\n");
    for (size_t i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; i++) {
        fprintf(out, "  %-38s %s\n", COMMANDS[i].usage, COMMANDS[i].help);
    }
    fprintf(out,
            "\n<chat> is one of:  me | @username | t.me/name | <chat id> | part of a chat title\n"
            "\nenvironment:\n"
            "  TBOX_HOME       data directory (default ~/.tbox)\n"
            "  TBOX_API_ID, TBOX_API_HASH\n"
            "                  API credentials (override ~/.tbox/config)\n"
            "  TBOX_TDJSON     path to libtdjson if not beside the executable\n"
            "  TBOX_DEBUG=1    print every JSON message exchanged with TDLib\n"
            "\nexit codes: 0 ok, 1 error, 2 usage, 3 not logged in\n");
}

int main(int argc, char **argv) {
    platform_init();

    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "help") == 0) {
        usage(argc < 2 ? stderr : stdout);
        return argc < 2 ? EXIT_USAGE : EXIT_OK;
    }
    if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "version") == 0) {
        printf("tbox %s\n", TBOX_VERSION);
        return EXIT_OK;
    }

    for (size_t i = 0; i < sizeof COMMANDS / sizeof COMMANDS[0]; i++) {
        if (strcmp(argv[1], COMMANDS[i].name) == 0) {
            return COMMANDS[i].run(argc - 2, argv + 2);
        }
    }
    fprintf(stderr, "tbox: unknown command '%s'\n\n", argv[1]);
    usage(stderr);
    return EXIT_USAGE;
}
