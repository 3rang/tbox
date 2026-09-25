/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * test_cli.c - CLI dispatcher matrix.
 *
 * Calls tbox_cli() directly with canned argv vectors and asserts the exit
 * code. Follows the current cmd.h API (TBOX_EXIT_OK / TBOX_ERROR /
 * TBOX_UNKNOWN_COMMAND). Headless by rule - no TDLib, no network.
 */

#include <stdio.h>
#include "cli/cli.h"
#include "cmd/cmd.h"

static int failures = 0;

static void check(const char *what, int got, int want)
{
    if (got == want) {
        printf("ok   %-28s exit %d\n", what, got);
    } else {
        printf("FAIL %-28s got %d want %d\n", what, got, want);
        failures++;
    }
}

int main(void)
{
    char *argv0[] = { "tbox" };

    /* no args -> help + generic error (exit 1) */
    check("no args", tbox_cli(1, argv0), TBOX_ERROR);

    /* help and version flags -> ok (exit 0) */
    {
        char *a[] = { "tbox", "-h" };
        check("-h", tbox_cli(2, a), TBOX_EXIT_OK);
    }
    {
        char *a[] = { "tbox", "--help" };
        check("--help", tbox_cli(2, a), TBOX_EXIT_OK);
    }
    {
        char *a[] = { "tbox", "-v" };
        check("-v", tbox_cli(2, a), TBOX_EXIT_OK);
    }
    {
        char *a[] = { "tbox", "--version" };
        check("--version", tbox_cli(2, a), TBOX_EXIT_OK);
    }

    /* anything else (commands not wired yet, junk, hidden cmds) -> help +
     * unknown-command (exit 2) */
    {
        char *a[] = { "tbox", "auth" };
        check("auth (not wired yet)", tbox_cli(2, a), TBOX_UNKNOWN_COMMAND);
    }
    {
        char *a[] = { "tbox", "status" };
        check("status (not wired yet)", tbox_cli(2, a), TBOX_UNKNOWN_COMMAND);
    }
    {
        char *a[] = { "tbox", "selftest" };
        check("selftest", tbox_cli(2, a), TBOX_UNKNOWN_COMMAND);
    }
    {
        char *a[] = { "tbox", "bogus" };
        check("unknown verb", tbox_cli(2, a), TBOX_UNKNOWN_COMMAND);
    }

    printf(failures ? "FAILED (%d)\n" : "test_cli: all green\n", failures);
    return failures ? 1 : 0;
}