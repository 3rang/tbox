/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * test_cli.c - CLI dispatcher matrix.
 *
 * Calls tbox_cli() directly with canned argv vectors and asserts the exit
 * code. Follows cmd.h semantics: 0 success, 1 runtime error (a command that
 * exists but cannot run here), 2 unknown command / bad usage. Headless by
 * rule - no TDLib, no network, and no writes to the real data directory
 * (scratch paths only).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cli/cli.h"
#include "cmd/cmd.h"
#include "util/datadir.h"
#include "util/lockfile.h"
#include "util/statusfile.h"

static int failures = 0;

static void check(const char *what, int got, int want)
{
    if (got == want) {
        printf("ok   %-34s exit %d\n", what, got);
    } else {
        printf("FAIL %-34s got %d want %d\n", what, got, want);
        failures++;
    }
}

/* A scratch data dir under the OS temp area, so status/serve never touch a
 * real %LOCALAPPDATA%\tbox while being exercised here. */
static void scratch_dir(char *out, size_t size)
{
    const char *base = getenv(
#ifdef _WIN32
        "TEMP"
#else
        "TMPDIR"
#endif
        );

    if (base == NULL || base[0] == '\0')
        base = ".";

    /* join, not snprintf: gcc's -Wformat-truncation cannot prove "%s/%s"
     * safe and this project builds with -Werror */
    (void)tbox_datadir_join(out, size, base, "tbox_cli_scratch");
}

/* Remove any leftovers a previous run left, so `status` starts from a
 * genuinely empty scratch dir. The directory itself is intentionally not
 * created: serve creates it, and status must not. */
static void drop_scratch(const char *dir)
{
    char file[512];

    if (tbox_status_path(file, sizeof file, dir) == 0)
        (void)remove(file);
    if (tbox_datadir_join(file, sizeof file, dir, "status.json.tmp") == 0)
        (void)remove(file);
    if (tbox_lockfile_path(file, sizeof file, dir) == 0)
        (void)remove(file);
}

int main(void)
{
    char scratch[512];
    char *noargs[]    = { "tbox" };
    char *h[]         = { "tbox", "-h" };
    char *help_flag[] = { "tbox", "--help" };
    char *v[]         = { "tbox", "-v" };
    char *ver[]       = { "tbox", "--version" };
    char *help_cmd[]  = { "tbox", "help" };
    char *auth[]      = { "tbox", "auth" };
    char *auth_x[]    = { "tbox", "auth", "ignored" };
    char *auth_data[] = { "tbox", "auth", "--data" };
    char *auth_ok[]   = { "tbox", "auth", "--data", "some\\dir" };
    char *selftest[]  = { "tbox", "selftest" };
    char *selftest_x[] = { "tbox", "selftest", "extra" };
    char *bogus[]     = { "tbox", "bogus" };

    scratch_dir(scratch, sizeof scratch);
    drop_scratch(scratch);   /* no leftovers, and not created either */

    /* usage */
    check("no args (usage)", tbox_cli(1, noargs), TBOX_UNKNOWN_COMMAND);

    /* flags */
    check("-h", tbox_cli(2, h), TBOX_EXIT_OK);
    check("--help", tbox_cli(2, help_flag), TBOX_EXIT_OK);
    check("-v", tbox_cli(2, v), TBOX_EXIT_OK);
    check("--version", tbox_cli(2, ver), TBOX_EXIT_OK);

    /* named commands */
    check("help command", tbox_cli(2, help_cmd), TBOX_EXIT_OK);
    check("selftest", tbox_cli(2, selftest), TBOX_EXIT_OK);
    check("selftest + arg (usage)", tbox_cli(3, selftest_x), TBOX_UNKNOWN_COMMAND);

    /* auth: a real command now. In a test binary nobody injected the TDLib
     * runner (headless by rule), so after argument parsing it reports
     * "not available in this build" -> runtime error (1). */
    check("auth (no runner)", tbox_cli(2, auth), TBOX_ERROR);
    check("auth + unknown arg (usage)", tbox_cli(3, auth_x), TBOX_UNKNOWN_COMMAND);
    check("auth --data (missing dir)", tbox_cli(3, auth_data), TBOX_UNKNOWN_COMMAND);
    check("auth --data <dir> (no runner)", tbox_cli(4, auth_ok), TBOX_ERROR);

    /* status: a real command now, but this scratch dir has no serve behind
     * it -> "not running" -> runtime error (1), not a usage error (2). */
    {
        char *status_scratch[] = { "tbox", "status", "--data", scratch };
        char *status_x[]       = { "tbox", "status", "nonsense" };
        char *status_data[]    = { "tbox", "status", "--data" };

        check("status (no serve running)", tbox_cli(4, status_scratch), TBOX_ERROR);
        check("status + unknown arg (usage)", tbox_cli(3, status_x), TBOX_UNKNOWN_COMMAND);
        check("status --data (missing dir)", tbox_cli(3, status_data), TBOX_UNKNOWN_COMMAND);
    }

    /* serve: with no TDLib runner injected it still takes the lock and
     * publishes status, then reports it has nothing to serve -> error (1). */
    {
        char *serve_scratch[] = { "tbox", "serve", "--data", scratch };
        char *serve_x[]       = { "tbox", "serve", "nonsense" };
        char *serve_data[]    = { "tbox", "serve", "--data" };

        check("serve (no TDLib)", tbox_cli(4, serve_scratch), TBOX_ERROR);
        check("serve + unknown arg (usage)", tbox_cli(3, serve_x), TBOX_UNKNOWN_COMMAND);
        check("serve --data (missing dir)", tbox_cli(3, serve_data), TBOX_UNKNOWN_COMMAND);
    }

    /* unknown */
    check("unknown verb", tbox_cli(2, bogus), TBOX_UNKNOWN_COMMAND);

    printf(failures ? "FAILED (%d)\n" : "test_cli: all green\n", failures);
    return failures ? 1 : 0;
}