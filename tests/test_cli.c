/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * test_cli.c - CLI dispatcher matrix.
 *
 * Calls tbox_cli() directly with canned argv vectors and asserts the exit
 * code. Follows cmd.h semantics: 0 success, 1 runtime error (stubs =
 * "not implemented"), 2 unknown command / usage. Headless by rule - no
 * TDLib, no network.
 */

#include <stdio.h>
#include "cli/cli.h"
#include "cmd/cmd.h"

static int failures = 0;

static void check(const char *what, int got, int want)
{
    if (got == want) {
        printf("ok   %-30s exit %d\n", what, got);
    } else {
        printf("FAIL %-30s got %d want %d\n", what, got, want);
        failures++;
    }
}

int main(void)
{
    char *noargs[]    = { "tbox" };
    char *h[]         = { "tbox", "-h" };
    char *help_flag[] = { "tbox", "--help" };
    char *v[]         = { "tbox", "-v" };
    char *ver[]       = { "tbox", "--version" };
    char *help_cmd[]  = { "tbox", "help" };
    char *auth[]      = { "tbox", "auth" };
    char *auth_x[]    = { "tbox", "auth", "ignored" };
    char *status[]    = { "tbox", "status" };
    char *selftest[]  = { "tbox", "selftest" };
    char *bogus[]     = { "tbox", "bogus" };

    /* usage */
    check("no args (usage)", tbox_cli(1, noargs), TBOX_UNKNOWN_COMMAND);

    /* flags */
    check("-h", tbox_cli(2, h), TBOX_EXIT_OK);
    check("--help", tbox_cli(2, help_flag), TBOX_EXIT_OK);
    check("-v", tbox_cli(2, v), TBOX_EXIT_OK);
    check("--version", tbox_cli(2, ver), TBOX_EXIT_OK);

    /* named commands */
    check("help command", tbox_cli(2, help_cmd), TBOX_EXIT_OK);

    /* stubs: reachable, honest "not implemented yet" -> runtime error (1) */
    check("auth (stub)", tbox_cli(2, auth), TBOX_ERROR);
    check("auth + extra arg (stub)", tbox_cli(3, auth_x), TBOX_ERROR);
    check("status (stub)", tbox_cli(2, status), TBOX_ERROR);
    check("selftest (stub)", tbox_cli(2, selftest), TBOX_ERROR);

    /* unknown */
    check("unknown verb", tbox_cli(2, bogus), TBOX_UNKNOWN_COMMAND);

    printf(failures ? "FAILED (%d)\n" : "test_cli: all green\n", failures);
    return failures ? 1 : 0;
}