/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * cmd/selftest.c - `tbox selftest`: a headless sanity check of the rules the
 * transports depend on.
 *
 * Deliberately pure logic only - no TDLib, no network, no filesystem writes,
 * no data directory. It answers "is this build's core behaving?" on a
 * machine that has never been logged in, which is exactly when a user runs
 * it. The file-backed behaviour (lock, status.json, atomic replace) is
 * covered by tests/test_statusfile.c instead.
 */

#define _CRT_SECURE_NO_WARNINGS   /* MSVC: strcpy in fixed-size buffers */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmd.h"
#include "core/index.h"
#include "util/datadir.h"
#include "util/statusfile.h"

static int failures = 0;

static void check(const char *what, int ok)
{
    if (ok) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static void check_str(const char *what, const char *got, const char *want)
{
    if (got != NULL && strcmp(got, want) == 0) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s (got \"%s\", want \"%s\")\n", what,
               got != NULL ? got : "(null)", want);
        failures++;
    }
}

/* The caption schema is the contract with every previously uploaded file. */
static void selftest_caption(void)
{
    tbox_archive_entry_t entry;
    char *json;

    check("caption: a valid file caption parses",
          tbox_caption_parse("{\"v\":1,\"type\":\"file\",\"name\":\"a/b.txt\","
                             "\"size\":7,\"mtime\":99,\"sha256\":\"abc\"}",
                             &entry) == 0);
    check_str("caption: name survives", entry.name, "a/b.txt");
    check("caption: size survives", entry.size == 7);
    check("caption: a foreign document is rejected",
          tbox_caption_parse("just some text", &entry) == -1);
    check("caption: a wrong version is rejected",
          tbox_caption_parse("{\"v\":2,\"name\":\"a\"}", &entry) == -1);

    memset(&entry, 0, sizeof entry);
    strcpy(entry.name, "a/b.txt");
    entry.size = 7;
    entry.mtime = 99;
    strcpy(entry.sha256, "abc");
    json = tbox_caption_build(&entry);
    check_str("caption: build is byte-compatible with v0.3", json,
              "{\"v\":1,\"type\":\"file\",\"name\":\"a/b.txt\","
              "\"size\":7,\"mtime\":99,\"sha256\":\"abc\"}");
    free(json);
}

/* Path handling is a security boundary: clients send hostile paths. */
static void selftest_paths(void)
{
    char name[TBOX_ARCHIVE_MAX_NAME];

    check("path: slashes collapse",
          tbox_archive_name("/a//b/", name, sizeof name) == 0);
    check_str("path: result", name, "a/b");
    check("path: a traversal is refused",
          tbox_archive_name("/a/../b", name, sizeof name) == -1);
    check("path: '..foo' is a legal name",
          tbox_archive_name("/..foo", name, sizeof name) == 0);

    check("path: data dir joins",
          tbox_datadir_join(name, sizeof name, "/data", "tdlib") == 0);
    check("path: a trailing separator is not doubled",
          tbox_datadir_join(name, sizeof name, "/data/", "tdlib") == 0);
    check("path: an over-long join is refused, not truncated",
          tbox_datadir_join(name, 8, "/data/tbox", "session.lock") == -1);
}

/* Implied directories are what make a flat message log look like a tree. */
static void selftest_tree(void)
{
    tbox_archive_t archive;
    tbox_archive_entry_t entry;
    tbox_archive_entry_t *children = NULL;
    size_t count = 0;

    tbox_archive_init(&archive);

    memset(&entry, 0, sizeof entry);
    strcpy(entry.name, "photos/2024/img.jpg");
    entry.message_id = 1;
    entry.size = 100;
    entry.mtime = 1750000000;
    strcpy(entry.sha256, "abc");
    (void)tbox_archive_add(&archive, &entry);

    /* an explicitly stored empty directory */
    memset(&entry, 0, sizeof entry);
    strcpy(entry.name, "empty");
    entry.message_id = TBOX_MSG_NONE;
    entry.is_dir = 1;
    (void)tbox_archive_add(&archive, &entry);

    check("tree: implied directories are synthesized",
          tbox_archive_stat(&archive, "photos/2024", &entry) == 0
              && entry.is_dir == 1);
    check("tree: a missing path is not found",
          tbox_archive_stat(&archive, "photos/1999", &entry) == -1);
    check("tree: the file is reachable by its full path",
          tbox_archive_stat(&archive, "photos/2024/img.jpg", &entry) == 0
              && entry.is_dir == 0 && entry.size == 100);

    check("tree: the root lists both top folders",
          tbox_archive_list(&archive, "/", &children, &count) == 0
              && count == 2);
    if (count == 2) {
        check_str("tree: children sort", children[0].name, "empty");
        check_str("tree: children sort", children[1].name, "photos");
    }
    free(children);

    check("tree: a folder lists its subfolder",
          tbox_archive_list(&archive, "photos", &children, &count) == 0
              && count == 1);
    if (count == 1) {
        check_str("tree: the subfolder keeps its full path", children[0].name,
                  "photos/2024");
        check("tree: the subfolder is a directory", children[0].is_dir == 1);
        check("tree: a directory owns no message",
              children[0].message_id == TBOX_MSG_NONE);
    }
    free(children);

    check("tree: an empty directory answers . and ..",
          tbox_archive_list(&archive, "empty", &children, &count) == 0
              && count == 2);
    if (count == 2) {
        check_str("tree: dot", children[0].name, ".");
        check_str("tree: dotdot", children[1].name, "..");
    }
    free(children);

    tbox_archive_free(&archive);
}

/* The state names are a wire contract with `tbox status` and the tray. */
static void selftest_states(void)
{
    check("state: ready round-trips",
          tbox_state_from_name(tbox_state_name(TBOX_STATE_READY))
              == TBOX_STATE_READY);
    check("state: unknown names are not guessed",
          tbox_state_from_name("something-else") == TBOX_STATE_UNKNOWN);
    check("state: 'ready' counts as running",
          tbox_state_is_running(TBOX_STATE_READY) == 1);
    check("state: 'stopped' does not",
          tbox_state_is_running(TBOX_STATE_STOPPED) == 0);
}

int tbox_cmd_selftest(int argc, char *argv[])
{
    if (argc > 2) {
        fprintf(stderr, "%s: unexpected argument '%s'\n", argv[1], argv[2]);
        return TBOX_UNKNOWN_COMMAND;
    }

    printf("tbox selftest: core rules only "
           "(no TDLib, no network, no writes)\n\n");

    selftest_caption();
    selftest_paths();
    selftest_tree();
    selftest_states();

    if (failures != 0) {
        printf("\nFAILED (%d)\n", failures);
        return TBOX_ERROR;
    }

    printf("\nselftest: all green\n");
    return TBOX_EXIT_OK;
}