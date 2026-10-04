/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * test_index.c - the archive ledger rules (core/index.c).
 *
 * These are the behaviours every consumer depends on: the CLI's `tbox ls`,
 * the FTP listing and later the SFTP one. They are the ported v0.3 rules,
 * so they are asserted exactly, not approximately:
 *   - caption parse fallbacks (foreign documents)
 *   - byte-compatible caption serialization (key order, no whitespace)
 *   - path normalization: fold separators, drop "." and empty, reject ".."
 *   - implied directories, lexicographic listing, empty-dir "." / ".."
 *
 * Headless by rule: no TDLib, no network, no filesystem.
 */

#define _CRT_SECURE_NO_WARNINGS   /* MSVC: strcpy in fixed-size buffers */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/index.h"

static int failures = 0;

/* long long params: message ids and sizes are 64-bit, and MSVC /W4 would
 * warn about narrowing them at every call site. */
static void check(const char *what, long long got, long long want)
{
    if (got == want) {
        printf("ok   %-46s %lld\n", what, got);
    } else {
        printf("FAIL %-46s got %lld want %lld\n", what, got, want);
        failures++;
    }
}

static void check_str(const char *what, const char *got, const char *want)
{
    if (got != NULL && want != NULL && strcmp(got, want) == 0) {
        printf("ok   %-46s %s\n", what, got);
    } else {
        printf("FAIL %-46s got \"%s\" want \"%s\"\n", what,
               got != NULL ? got : "(null)", want != NULL ? want : "(null)");
        failures++;
    }
}

/* ---- path normalization --------------------------------------------------- */

static void test_names(void)
{
    char buf[TBOX_ARCHIVE_MAX_NAME];

    check("name: root", tbox_archive_name("/", buf, sizeof buf), 0);
    check_str("name: root is empty", buf, "");

    check("name: leading+trailing slashes", tbox_archive_name("/a/b/", buf, sizeof buf), 0);
    check_str("name: strips slashes", buf, "a/b");

    check("name: double slashes + dot", tbox_archive_name("/a//./b/", buf, sizeof buf), 0);
    check_str("name: collapses empty + dot", buf, "a/b");

    check("name: backslashes fold", tbox_archive_name("a\\b\\c", buf, sizeof buf), 0);
    check_str("name: folded result", buf, "a/b/c");

    check("name: dotfile is a name", tbox_archive_name("/.hidden", buf, sizeof buf), 0);
    check_str("name: dotfile kept", buf, ".hidden");

    /* only a WHOLE ".." segment is traversal: "/..foo" is a legal name */
    check("name: leading .. is traversal", tbox_archive_name("/../etc", buf, sizeof buf), -1);
    check("name: embedded .. is traversal", tbox_archive_name("/a/../b", buf, sizeof buf), -1);
    check("name: trailing .. is traversal", tbox_archive_name("/a/..", buf, sizeof buf), -1);
    check("name: backslash .. is traversal", tbox_archive_name("a\\..\\b", buf, sizeof buf), -1);
    check("name: ..foo is legal", tbox_archive_name("/..foo", buf, sizeof buf), 0);
    check_str("name: ..foo kept", buf, "..foo");

    check("name: null path rejected", tbox_archive_name(NULL, buf, sizeof buf), -1);

    /* buffer too small must fail, not truncate */
    {
        char small[8];
        check("name: too-small buffer", tbox_archive_name("/aaaa/bbbb", small, sizeof small), -1);
    }
}

/* ---- caption schema -------------------------------------------------------- */

static void test_caption_parse(void)
{
    tbox_archive_entry_t e;

    /* the exact v0.3 file caption */
    check("caption: valid file",
          tbox_caption_parse("{\"v\":1,\"type\":\"file\",\"name\":\"docs/a.txt\","
                             "\"size\":1234,\"mtime\":1750000000,"
                             "\"sha256\":\"abc123\"}", &e), 0);
    check_str("caption: name", e.name, "docs/a.txt");
    check("caption: size", e.size, 1234);
    check("caption: mtime", e.mtime, 1750000000LL);
    check_str("caption: sha256", e.sha256, "abc123");
    check("caption: is_dir", e.is_dir, 0);
    check("caption: is_tbox", e.is_tbox, 1);

    /* dir markers carry only v/type/name */
    check("caption: valid dir",
          tbox_caption_parse("{\"v\":1,\"type\":\"dir\",\"name\":\"docs\"}", &e), 0);
    check_str("caption: dir name", e.name, "docs");
    check("caption: dir is_dir", e.is_dir, 1);
    check("caption: dir is_tbox", e.is_tbox, 1);

    /* trailing separator on a dir name is trimmed */
    check("caption: dir trailing slash",
          tbox_caption_parse("{\"v\":1,\"type\":\"dir\",\"name\":\"docs/\"}", &e), 0);
    check_str("caption: dir trimmed", e.name, "docs");

    /* defaults: missing type = file, missing size = 0, missing mtime = -1 */
    check("caption: type defaults to file",
          tbox_caption_parse("{\"v\":1,\"name\":\"x.txt\"}", &e), 0);
    check("caption: default is_dir", e.is_dir, 0);
    check("caption: default size", e.size, 0);
    check("caption: default mtime", e.mtime, -1);
    check_str("caption: default sha empty", e.sha256, "");
    check("caption: no sha+mtime = foreign", e.is_tbox, 0);

    /* every "not ours" case -> -1 (caller falls back to the document) */
    check("caption: NULL rejected", tbox_caption_parse(NULL, &e), -1);
    check("caption: empty rejected", tbox_caption_parse("", &e), -1);
    check("caption: garbage rejected", tbox_caption_parse("not json", &e), -1);
    check("caption: array rejected", tbox_caption_parse("[1,2]", &e), -1);
    check("caption: plain text rejected", tbox_caption_parse("hello world", &e), -1);
    check("caption: wrong version rejected", tbox_caption_parse("{\"v\":2,\"name\":\"a\"}", &e), -1);
    check("caption: missing version rejected", tbox_caption_parse("{\"name\":\"a\"}", &e), -1);
    check("caption: missing name rejected", tbox_caption_parse("{\"v\":1,\"type\":\"file\"}", &e), -1);
    check("caption: traversal name rejected", tbox_caption_parse("{\"v\":1,\"name\":\"../x\"}", &e), -1);

    /* a rejected caption must not have scribbled on the caller's struct */
    memset(&e, 0xAB, sizeof e);
    check("caption: rejected again", tbox_caption_parse("{bad", &e), -1);
    check("caption: struct untouched", (long long)(unsigned char)e.name[0], 0xAB);
}

static void test_caption_build(void)
{
    tbox_archive_entry_t e;
    char *json;

    memset(&e, 0, sizeof e);
    strcpy(e.name, "docs/a.txt");
    e.message_id = 42;
    e.is_dir = 0;
    e.size = 1234;
    e.mtime = 1750000000LL;
    strcpy(e.sha256, "abc123");

    json = tbox_caption_build(&e);
    check_str("caption: build is byte-compatible", json,
              "{\"v\":1,\"type\":\"file\",\"name\":\"docs/a.txt\","
              "\"size\":1234,\"mtime\":1750000000,\"sha256\":\"abc123\"}");
    free(json);

    /* directories emit only the three keys */
    memset(&e, 0, sizeof e);
    strcpy(e.name, "docs");
    e.is_dir = 1;
    json = tbox_caption_build(&e);
    check_str("caption: dir build is minimal", json,
              "{\"v\":1,\"type\":\"dir\",\"name\":\"docs\"}");
    free(json);

    /* round-trip through the parser */
    memset(&e, 0, sizeof e);
    strcpy(e.name, "a/b/c.bin");
    e.size = 7;
    e.mtime = 99;
    strcpy(e.sha256, "deadbeef");
    json = tbox_caption_build(&e);
    {
        tbox_archive_entry_t back;

        check("caption: round-trip parses", tbox_caption_parse(json, &back), 0);
        check_str("caption: round-trip name", back.name, "a/b/c.bin");
        check("caption: round-trip size", back.size, 7);
        check("caption: round-trip mtime", back.mtime, 99);
        check_str("caption: round-trip sha", back.sha256, "deadbeef");
    }
    free(json);
}

/* ---- index ----------------------------------------------------------------- */

static void add_file(tbox_archive_t *ar, const char *name, long long msg_id,
                     long long size)
{
    tbox_archive_entry_t e;

    memset(&e, 0, sizeof e);
    snprintf(e.name, sizeof e.name, "%s", name);
    e.message_id = msg_id;
    e.size = size;
    e.mtime = 1750000000LL;
    strcpy(e.sha256, "abc123");
    if (tbox_archive_add(ar, &e) != 0) {
        printf("FAIL add(%s) failed\n", name);
        failures++;
    }
}

static void add_dir(tbox_archive_t *ar, const char *name, long long msg_id)
{
    tbox_archive_entry_t e;

    memset(&e, 0, sizeof e);
    snprintf(e.name, sizeof e.name, "%s", name);
    e.message_id = msg_id;
    e.is_dir = 1;
    if (tbox_archive_add(ar, &e) != 0) {
        printf("FAIL add_dir(%s) failed\n", name);
        failures++;
    }
}

/* listing helpers ----------------------------------------------------------- */

static tbox_archive_entry_t *list_of(tbox_archive_t *ar, const char *path,
                                     size_t *count)
{
    tbox_archive_entry_t *out = NULL;
    size_t cap = 0;

    *count = 0;
    if (tbox_archive_list(ar, path, &out, &cap) != 0)
        return NULL;
    *count = cap;

    return out;
}

static void test_find_and_dedupe(void)
{
    tbox_archive_t ar;
    const tbox_archive_entry_t *hit;

    tbox_archive_init(&ar);
    add_file(&ar, "a.txt", 10, 100);

    hit = tbox_archive_find(&ar, "/a.txt");
    check("find: hit is non-null", hit != NULL, 1);
    if (hit != NULL)
        check("find: message id", hit->message_id, 10);
    check("find: miss is null", tbox_archive_find(&ar, "/nope.txt") == NULL, 1);
    check("find: traversal is rejected", tbox_archive_find(&ar, "/../a.txt") == NULL, 1);

    /* re-adding the same name replaces in place (newest wins) */
    add_file(&ar, "a.txt", 11, 200);
    check("dedupe: count stays 1", (int)ar.count, 1);
    hit = tbox_archive_find(&ar, "a.txt");
    if (hit != NULL) {
        check("dedupe: newest message id", hit->message_id, 11);
        check("dedupe: newest size", hit->size, 200);
    }

    tbox_archive_free(&ar);
    check("free: count reset", (int)ar.count, 0);
    check("free: items reset", ar.items == NULL, 1);
}

static void test_materialize(void)
{
    tbox_archive_t ar;
    const tbox_archive_entry_t *hit;

    tbox_archive_init(&ar);
    add_file(&ar, "backup/data/x.csv", 1, 10);
    add_file(&ar, "backup/readme.md", 2, 20);
    tbox_archive_materialize(&ar);

    /* the whole prefix chain must exist: backup, backup/data */
    hit = tbox_archive_find(&ar, "backup");
    check("implied: backup exists", hit != NULL, 1);
    if (hit != NULL) {
        check("implied: backup is a dir", hit->is_dir, 1);
        check("implied: backup has no message", hit->message_id, TBOX_MSG_NONE);
    }
    hit = tbox_archive_find(&ar, "backup/data");
    check("implied: backup/data exists", hit != NULL, 1);
    check("implied: backup/data is a dir",
          hit != NULL ? hit->is_dir : 0, 1);
    check("implied: deeper prefix missing", tbox_archive_find(&ar, "backup/data/x") == NULL, 1);

    /* an explicit marker must not be duplicated */
    tbox_archive_free(&ar);
    tbox_archive_init(&ar);
    add_dir(&ar, "backup", 99);
    add_file(&ar, "backup/x", 1, 10);
    tbox_archive_materialize(&ar);
    hit = tbox_archive_find(&ar, "backup");
    check("implied: marker wins over implied",
          hit != NULL ? hit->message_id : -1, 99);
    check("implied: no duplicate marker", (int)ar.count, 2);

    /* idempotent */
    {
        size_t before = ar.count;
        tbox_archive_materialize(&ar);
        check("implied: idempotent", (int)ar.count, (int)before);
    }

    tbox_archive_free(&ar);
}

static void test_list_root(void)
{
    tbox_archive_t ar;
    tbox_archive_entry_t *out;
    size_t n = 0;

    tbox_archive_init(&ar);
    add_file(&ar, "b.txt", 1, 10);
    add_file(&ar, "a.txt", 2, 20);
    add_file(&ar, "backup/data/x.csv", 3, 30);
    add_file(&ar, "az.txt", 4, 40);

    /* root: 4 children (backup is implied as a dir), lexicographic:
     * a.txt, az.txt, b.txt, backup - files and dirs interleaved. */
    out = list_of(&ar, "/", &n);
    check("list root: count", (int)n, 4);
    if (n == 4) {
        check_str("list root: [0]", out[0].name, "a.txt");
        check_str("list root: [1]", out[1].name, "az.txt");
        check_str("list root: [2]", out[2].name, "b.txt");
        check_str("list root: [3]", out[3].name, "backup");
        check("list root: implied dir flagged", out[3].is_dir, 1);
        check("list root: dir size is 0", out[3].size, 0);
        check("list root: file dir flag", out[0].is_dir, 0);
        check("list root: file keeps message id", out[0].message_id, 2);
        /* implied from a deeper entry: no message of its own, so no date */
        check("list root: implied dir has no date", out[3].mtime, -1);
    }
    free(out);

    tbox_archive_free(&ar);

    /*
     * A directory that has its own marker message shows that message's date.
     * A listing that claims Jan 1970 for a folder the user can plainly see is
     * just wrong, and the marker is the message that really did create it.
     */
    tbox_archive_init(&ar);
    add_file(&ar, "pics/a.png", 1, 10);
    add_file(&ar, "pics/b.png", 2, 20);
    {
        tbox_archive_entry_t e;

        memset(&e, 0, sizeof e);
        snprintf(e.name, sizeof e.name, "%s", "pics");
        e.message_id = 77;
        e.is_dir = 1;
        e.mtime = 1780000000LL;
        if (tbox_archive_add(&ar, &e) != 0) {
            printf("FAIL add(pics marker) failed\n");
            failures++;
        }
    }

    out = list_of(&ar, "/", &n);
    check("list marked: count", (int)n, 1);
    if (n == 1) {
        check_str("list marked: name", out[0].name, "pics");
        check("list marked: is a dir", out[0].is_dir, 1);
        check("list marked: marker message id", out[0].message_id, 77);
        check("list marked: marker date", out[0].mtime, 1780000000LL);
        check("list marked: dir size is still 0", out[0].size, 0);
    }
    free(out);

    tbox_archive_free(&ar);
}

static void test_list_subdir(void)
{
    tbox_archive_t ar;
    tbox_archive_entry_t *out;
    size_t n = 0;

    tbox_archive_init(&ar);
    add_file(&ar, "backup/data/x.csv", 1, 10);
    add_file(&ar, "backup/data/y.csv", 2, 20);
    add_file(&ar, "backup/z.txt", 3, 30);
    add_dir(&ar, "backup/empty", 4);

    /* /backup -> data (dir), empty (dir), z.txt */
    out = list_of(&ar, "/backup", &n);
    check("list dir: count", (int)n, 3);
    if (n == 3) {
        check_str("list dir: [0]", out[0].name, "backup/data");
        check_str("list dir: [1]", out[1].name, "backup/empty");
        check_str("list dir: [2]", out[2].name, "backup/z.txt");
        check("list dir: [1] is the marker dir", out[1].is_dir, 1);
        check("list dir: [1] keeps its message id", out[1].message_id, 4);
        /* data exists only because its children do, so it has no date */
        check("list dir: [0] implied dir has no date", out[0].mtime, -1);
        /* the marker for empty carries no date either */
        check("list dir: [1] marker without a date", out[1].mtime, 0);
        check("list dir: [2] file keeps its date", out[2].mtime, 1750000000LL);
    }
    free(out);

    /* /backup/data -> x.csv, y.csv (full logical names, not basenames) */
    out = list_of(&ar, "backup/data", &n);
    check("list nested: count", (int)n, 2);
    if (n == 2) {
        check_str("list nested: [0]", out[0].name, "backup/data/x.csv");
        check_str("list nested: [1]", out[1].name, "backup/data/y.csv");
        check("list nested: sizes", (int)out[1].size, 20);
    }
    free(out);

    /* the directory marker itself is never listed inside itself */
    out = list_of(&ar, "/backup/empty", &n);
    check("list empty: count", (int)n, 2);
    if (n == 2) {
        check_str("list empty: dot", out[0].name, ".");
        check_str("list empty: dotdot", out[1].name, "..");
        check("list empty: dot is a dir", out[0].is_dir, 1);
        check("list empty: dot has no message", out[0].message_id, TBOX_MSG_NONE);
    }
    free(out);

    /* an empty root also answers . and .. */
    tbox_archive_free(&ar);
    tbox_archive_init(&ar);
    out = list_of(&ar, "/", &n);
    check("list empty root: count", (int)n, 2);
    free(out);

    tbox_archive_free(&ar);
}

static void test_list_errors(void)
{
    tbox_archive_t ar;
    tbox_archive_entry_t *out = NULL;
    size_t cap = 0;

    tbox_archive_init(&ar);
    add_file(&ar, "a.txt", 1, 10);

    /* listing a file is not a directory listing */
    check("list: file path rejected", tbox_archive_list(&ar, "/a.txt", &out, &cap), -2);
    check("list: rejected leaves out NULL", out == NULL, 1);

    /* unknown directory */
    check("list: unknown dir", tbox_archive_list(&ar, "/nope", &out, &cap), -2);

    /* traversal never lists */
    check("list: traversal rejected", tbox_archive_list(&ar, "/../..", &out, &cap), -1);
    check("list: traversal leaves out NULL", out == NULL, 1);

    check("list: null args rejected", tbox_archive_list(&ar, NULL, &out, &cap), -1);

    tbox_archive_free(&ar);
}

static void test_stat(void)
{
    tbox_archive_t ar;
    tbox_archive_entry_t e;

    tbox_archive_init(&ar);
    add_file(&ar, "backup/data/x.csv", 1, 10);
    add_dir(&ar, "backup/empty", 2);

    check("stat: file found", tbox_archive_stat(&ar, "/backup/data/x.csv", &e), 0);
    check("stat: file is_dir", e.is_dir, 0);
    check("stat: file size", (int)e.size, 10);

    check("stat: implied dir found", tbox_archive_stat(&ar, "/backup/data", &e), 0);
    check("stat: implied dir is_dir", e.is_dir, 1);

    check("stat: marker dir found", tbox_archive_stat(&ar, "/backup/empty", &e), 0);
    check("stat: marker dir is_dir", e.is_dir, 1);

    check("stat: root found", tbox_archive_stat(&ar, "/", &e), 0);
    check("stat: root is_dir", e.is_dir, 1);
    check_str("stat: root name empty", e.name, "");

    check("stat: miss", tbox_archive_stat(&ar, "/nope", &e), -1);
    check("stat: traversal rejected", tbox_archive_stat(&ar, "/../x", &e), -1);

    tbox_archive_free(&ar);
}

static void test_sort(void)
{
    tbox_archive_t ar;
    size_t i;

    tbox_archive_init(&ar);
    add_file(&ar, "c", 1, 1);
    add_file(&ar, "a", 2, 2);
    add_file(&ar, "b", 3, 3);
    add_dir(&ar, "A", 4);

    tbox_archive_sort(&ar);
    check("sort: count", (int)ar.count, 4);
    for (i = 0; i < ar.count; i++)
        check_str("sort: order", ar.items[i].name,
                  i == 0 ? "A" : i == 1 ? "a" : i == 2 ? "b" : "c");

    tbox_archive_free(&ar);
}

/* A small realistic archive, as a client would see it after a v0.3 upload. */
static void test_scenario(void)
{
    tbox_archive_t ar;
    tbox_archive_entry_t *out;
    size_t n = 0;

    tbox_archive_init(&ar);

    /* feed it through the caption parser, the way tdscan will */
    {
        static const char *captions[] = {
            "{\"v\":1,\"type\":\"file\",\"name\":\"photos/2024/img_001.jpg\","
            "\"size\":204800,\"mtime\":1750000001,\"sha256\":\"aaa\"}",
            "{\"v\":1,\"type\":\"dir\",\"name\":\"photos/2024\"}",
            "{\"v\":1,\"type\":\"file\",\"name\":\"notes.txt\",\"size\":12,"
            "\"mtime\":1750000002,\"sha256\":\"bbb\"}",
            "{\"v\":1,\"type\":\"file\",\"name\":\"photos/2024/raw.zip\","
            "\"size\":900,\"mtime\":1750000003,\"sha256\":\"ccc\"}",
        };
        size_t i;

        for (i = 0; i < sizeof captions / sizeof captions[0]; i++) {
            tbox_archive_entry_t e;

            if (tbox_caption_parse(captions[i], &e) != 0) {
                printf("FAIL scenario: caption %u did not parse\n", (unsigned)i);
                failures++;
                continue;
            }
            e.message_id = (long long)i + 1;
            (void)tbox_archive_add(&ar, &e);
        }
    }

    /* a foreign document with no usable caption */
    add_file(&ar, "message-777", 777, 4242);

    out = list_of(&ar, "/", &n);
    check("scenario: root count", (int)n, 3);
    if (n == 3) {
        /* "photos/2024" is a child of photos, not of the root */
        check_str("scenario: root [0]", out[0].name, "message-777");
        check_str("scenario: root [1]", out[1].name, "notes.txt");
        check_str("scenario: root [2]", out[2].name, "photos");
        check("scenario: root dir is a dir", out[2].is_dir, 1);
    }
    free(out);

    out = list_of(&ar, "photos/2024", &n);
    check("scenario: photos/2024 count", (int)n, 2);
    if (n == 2) {
        check_str("scenario: photos/2024 [0]", out[0].name, "photos/2024/img_001.jpg");
        check_str("scenario: photos/2024 [1]", out[1].name, "photos/2024/raw.zip");
        check("scenario: file size survives", (int)out[0].size, 204800);
        check_str("scenario: file sha survives", out[0].sha256, "aaa");
        check("scenario: file is_tbox", out[0].is_tbox, 1);
    }
    free(out);

    tbox_archive_free(&ar);
}

int main(void)
{
    test_names();
    test_caption_parse();
    test_caption_build();
    test_find_and_dedupe();
    test_materialize();
    test_list_root();
    test_list_subdir();
    test_list_errors();
    test_stat();
    test_sort();
    test_scenario();

    printf(failures ? "FAILED (%d)\n" : "test_index: all green\n", failures);
    return failures ? 1 : 0;
}