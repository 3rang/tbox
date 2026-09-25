/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * test_cli.c - CLI dispatcher matrix (Step 1 gate).
 *
 * Calls tbox_cli() directly with canned argv vectors and asserts the exit
 * code, plus the internal registry rules: exposed commands are reachable
 * from argv, exposed=0 commands exist internally but are NOT selectable
 * from the command line. Headless by rule - no TDLib, no network.
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

    /* no args -> usage (exit 2) */
    check("no args", tbox_cli(1, argv0), TBOX_EXIT_USAGE);

    /* help: flags and command form -> ok (exit 0) */
    {
        char *a[] = { "tbox", "-h" };
        check("-h", tbox_cli(2, a), TBOX_EXIT_OK);
    }
    {
        char *a[] = { "tbox", "--help" };
        check("--help", tbox_cli(2, a), TBOX_EXIT_OK);
    }
    {
        char *a[] = { "tbox", "help" };
        check("help", tbox_cli(2, a), TBOX_EXIT_OK);
    }

    /* version: flags -> ok (exit 0) */
    {
        char *a[] = { "tbox", "-v" };
        check("-v", tbox_cli(2, a), TBOX_EXIT_OK);
    }
    {
        char *a[] = { "tbox", "--version" };
        check("--version", tbox_cli(2, a), TBOX_EXIT_OK);
    }

    /* exposed command stubs -> not-implemented (exit 5) */
    {
        char *a[] = { "tbox", "auth" };
        check("auth (stub)", tbox_cli(2, a), TBOX_EXIT_NOT_IMPL);
    }
    {
        char *a[] = { "tbox", "status" };
        check("status (stub)", tbox_cli(2, a), TBOX_EXIT_NOT_IMPL);
    }

    /* unknown verb -> usage (exit 2) */
    {
        char *a[] = { "tbox", "bogus" };
        check("unknown verb", tbox_cli(2, a), TBOX_EXIT_USAGE);
    }
    /* internal-only command must NOT be reachable from argv */
    {
        char *a[] = { "tbox", "selftest" };
        check("selftest hidden from CLI", tbox_cli(2, a), TBOX_EXIT_USAGE);
    }

    /* internal registry finds hidden commands and rejects unknown names */
    if (tbox_cmd_by_name("selftest") != NULL)
        printf("ok   registry finds internal 'selftest'\n");
    else { printf("FAIL registry misses internal 'selftest'\n"); failures++; }

    if (tbox_cmd_by_name("auth") != NULL)
        printf("ok   registry finds exposed 'auth'\n");
    else { printf("FAIL registry misses exposed 'auth'\n"); failures++; }

    if (tbox_cmd_by_name("bogus") == NULL)
        printf("ok   registry rejects unknown name\n");
    else { printf("FAIL registry accepts 'bogus'\n"); failures++; }

    printf(failures ? "FAILED (%d)\n" : "test_cli: all green\n", failures);
    return failures ? 1 : 0;
}