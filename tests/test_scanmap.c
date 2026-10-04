/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * test_scanmap.c - one Saved Messages document -> one archive entry
 * (core/scanmap.c).
 *
 * This is the decision the whole browse/download experience rests on: which
 * name a message gets, and whether we claim it as one of ours. It is
 * asserted against the v0.3 rules, including the awkward corners:
 *   - our caption wins, but only when it carries a usable name
 *   - a foreign document is named after the document, else after its message
 *   - a caption without a name / with a bad version / with junk is foreign
 *   - mtime and size fall back so no listing shows 1970 or 0 for a real file
 *   - is_tbox stays 0 for anything we did not write, so write ops can refuse
 *
 * Headless by rule: no TDLib, no network, no filesystem.
 */

#define _CRT_SECURE_NO_WARNINGS   /* MSVC: strcpy in fixed-size buffers */

#include <stdio.h>
#include <stdlib.h>   /* free */
#include <string.h>

#include "core/scanmap.h"

static int failures = 0;

/* long long params: message ids and sizes are 64-bit, and MSVC /W4 would
 * warn about narrowing them at every call site. */
static void check(const char *what, long long got, long long want)
{
    if (got == want) {
        printf("ok   %-52s %lld\n", what, got);
    } else {
        printf("FAIL %-52s got %lld want %lld\n", what, got, want);
        failures++;
    }
}

static void check_str(const char *what, const char *got, const char *want)
{
    if (got != NULL && want != NULL && strcmp(got, want) == 0) {
        printf("ok   %-52s %s\n", what, got);
    } else {
        printf("FAIL %-52s got \"%s\" want \"%s\"\n", what,
               got != NULL ? got : "(null)", want != NULL ? want : "(null)");
        failures++;
    }
}

#define FILE_CAP "{\"v\":1,\"type\":\"file\",\"name\":\"a/b.txt\",\"size\":11," \
                 "\"mtime\":1700000000,\"sha256\":\"ab12\"}"
#define DIR_CAP "{\"v\":1,\"type\":\"dir\",\"name\":\"photos\"}"

/* ---- foreign documents ------------------------------------------------------ */

static void test_foreign(void)
{
    tbox_archive_entry_t e;

    /* the ordinary case: a file we did not upload */
    check("foreign: returns 0",
          tbox_scanmap_entry(77, "just a note", "report.pdf", 2048, 1700000123,
                             555, &e), 0);
    check_str("foreign: named after the document", e.name, "report.pdf");
    check("foreign: is_tbox is 0", e.is_tbox, 0);
    check("foreign: is_dir is 0", e.is_dir, 0);
    check("foreign: no sha256", e.sha256[0], 0);
    check("foreign: size from the document", e.size, 2048);
    check("foreign: mtime from the message date", e.mtime, 1700000123);
    check("foreign: message id carried", e.message_id, 77);
    check("foreign: remote file id carried", e.file_id, 555);

    /* no filename: the message id is the only unique thing we have */
    check("no name: returns 0",
          tbox_scanmap_entry(78, NULL, NULL, 10, 5, 0, &e), 0);
    check_str("no name: falls back to message-<id>", e.name, "message-78");
    check("no name: remote file id 0 stays 0", e.file_id, 0);

    /* an empty filename is no filename */
    check("empty name: falls back",
          tbox_scanmap_entry(79, "", "", 10, 5, 0, &e), 0);
    check_str("empty name: falls back to message-<id>", e.name, "message-79");

    /* a filename that is not a legal archive name */
    check("traversal name: refused",
          tbox_scanmap_entry(80, NULL, "..", 10, 5, 0, &e), 0);
    check_str("traversal name: falls back to message-<id>", e.name, "message-80");

    check("root name: refused",
          tbox_scanmap_entry(81, NULL, "/", 10, 5, 0, &e), 0);
    check_str("root name: falls back to message-<id>", e.name, "message-81");

    /* separators are folded, exactly as everywhere else in the archive */
    check("backslash name: normalized",
          tbox_scanmap_entry(82, NULL, "sub\\dir\\a.txt", 10, 5, 0, &e), 0);
    check_str("backslash name: becomes a path", e.name, "sub/dir/a.txt");

    check("absolute name: normalized",
          tbox_scanmap_entry(83, NULL, "/etc/a.txt", 10, 5, 0, &e), 0);
    check_str("absolute name: loses the leading slash", e.name, "etc/a.txt");

    /* too long to store */
    {
        char huge[TBOX_ARCHIVE_MAX_NAME + 64];

        memset(huge, 'x', sizeof huge - 1);
        huge[sizeof huge - 1] = '\0';
        check("overlong name: refused",
              tbox_scanmap_entry(84, NULL, huge, 10, 5, 0, &e), 0);
        check_str("overlong name: falls back to message-<id>", e.name,
                  "message-84");
    }

    /* Telegram did not tell us the size or the date */
    check("no metadata: returns 0",
          tbox_scanmap_entry(85, NULL, "a.txt", 0, 0, 0, &e), 0);
    check("no metadata: size stays 0", e.size, 0);
    check("no metadata: mtime is -1 (unknown)", e.mtime, -1);
}

/* ---- our own captions ------------------------------------------------------- */

static void test_caption_file(void)
{
    tbox_archive_entry_t e;

    check("caption file: returns 0",
          tbox_scanmap_entry(101, FILE_CAP, "upload.bin", 999, 1700000999,
                             42, &e), 0);
    check_str("caption file: the caption name wins", e.name, "a/b.txt");
    check("caption file: is_tbox is 1", e.is_tbox, 1);
    check("caption file: is_dir is 0", e.is_dir, 0);
    check("caption file: the document is not a directory", e.size, 11);
    check("caption file: the caption mtime wins", e.mtime, 1700000000);
    check_str("caption file: the sha256 is kept", e.sha256, "ab12");
    check("caption file: message id carried", e.message_id, 101);
    check("caption file: remote file id carried", e.file_id, 42);

    /* no size in the caption: the document knows it */
    check("caption without size: uses the document",
          tbox_scanmap_entry(102,
                             "{\"v\":1,\"type\":\"file\",\"name\":\"a.txt\","
                             "\"mtime\":5,\"sha256\":\"cd\"}",
                             "upload.bin", 4096, 9, 43, &e), 0);
    check_str("caption without size: keeps the name", e.name, "a.txt");
    check("caption without size: size falls back", e.size, 4096);

    /* no mtime in the caption: not ours by the v0.3 rule, but still dated */
    check("caption without mtime: returns 0",
          tbox_scanmap_entry(103,
                             "{\"v\":1,\"type\":\"file\",\"name\":\"a.txt\","
                             "\"size\":7,\"sha256\":\"ef\"}",
                             "upload.bin", 999, 1700000777, 44, &e), 0);
    check_str("caption without mtime: keeps the name", e.name, "a.txt");
    check("caption without mtime: is_tbox stays 0", e.is_tbox, 0);
    check_str("caption without mtime: sha256 still kept", e.sha256, "ef");
    check("caption without mtime: date fills the gap", e.mtime, 1700000777);
    check("caption without mtime: size from the caption", e.size, 7);

    /* no size and no date at all */
    check("bare caption: returns 0",
          tbox_scanmap_entry(104,
                             "{\"v\":1,\"type\":\"file\",\"name\":\"a.txt\"}",
                             "up.bin", 0, 0, 0, &e), 0);
    check("bare caption: is_tbox is 0", e.is_tbox, 0);
    check("bare caption: size 0", e.size, 0);
    check("bare caption: mtime -1", e.mtime, -1);
}

static void test_caption_dir(void)
{
    tbox_archive_entry_t e;

    check("dir caption: returns 0",
          tbox_scanmap_entry(111, DIR_CAP, "marker.txt", 1, 1700000111, 51, &e),
          0);
    check_str("dir caption: the name is kept", e.name, "photos");
    check("dir caption: is_dir is 1", e.is_dir, 1);
    check("dir caption: is_tbox is 1", e.is_tbox, 1);
    check("dir caption: no size", e.size, 0);
    /* the marker carries no mtime, so the send date keeps listings sane */
    check("dir caption: date fills the gap", e.mtime, 1700000111);
    check("dir caption: remote file id carried", e.file_id, 51);

    /* a trailing separator in a dir caption is trimmed, as v0.3 did */
    check("dir caption with slash: returns 0",
          tbox_scanmap_entry(112,
                             "{\"v\":1,\"type\":\"dir\",\"name\":\"photos/\"}",
                             "marker.txt", 1, 5, 52, &e), 0);
    check_str("dir caption with slash: trimmed", e.name, "photos");
    check("dir caption with slash: is_dir is 1", e.is_dir, 1);

    /* no date either: unknown stays unknown */
    check("dir caption undated: mtime -1",
          (tbox_scanmap_entry(113, DIR_CAP, "m.txt", 1, 0, 53, &e), e.mtime),
          -1);
}

/* ---- captions we must not trust -------------------------------------------- */

static void test_untrusted_captions(void)
{
    tbox_archive_entry_t e;

    /* no name -> the document names it */
    check("nameless caption: uses the document",
          tbox_scanmap_entry(121,
                             "{\"v\":1,\"type\":\"file\",\"size\":1}", "real.txt",
                             12, 5, 0, &e), 0);
    check_str("nameless caption: named after the document", e.name, "real.txt");
    check("nameless caption: is_tbox is 0", e.is_tbox, 0);

    /* empty name -> the same */
    check("empty-name caption: uses the document",
          tbox_scanmap_entry(122,
                             "{\"v\":1,\"type\":\"file\",\"name\":\"\"}",
                             "real.txt", 12, 5, 0, &e), 0);
    check_str("empty-name caption: named after the document", e.name,
              "real.txt");

    /* not JSON at all */
    check("text message: uses the document",
          tbox_scanmap_entry(123, "see you tomorrow", "real.txt", 12, 5, 0, &e),
          0);
    check_str("text message: named after the document", e.name, "real.txt");
    check("text message: is_tbox is 0", e.is_tbox, 0);
    check("text message: message id carried", e.message_id, 123);

    /* a JSON array is not a caption */
    check("array caption: uses the document",
          tbox_scanmap_entry(124, "[1,2,3]", "real.txt", 12, 5, 0, &e), 0);
    check_str("array caption: named after the document", e.name, "real.txt");

    /* a future version is not one of ours (v0.3 rule: v must be 1) */
    check("future caption version: uses the document",
          tbox_scanmap_entry(125,
                             "{\"v\":2,\"type\":\"file\",\"name\":\"x.txt\","
                             "\"sha256\":\"aa\",\"mtime\":1}",
                             "real.txt", 12, 5, 0, &e), 0);
    check_str("future caption version: named after the document", e.name,
              "real.txt");
    check("future caption version: is_tbox is 0", e.is_tbox, 0);

    /* a caption naming ".." is not usable as a path */
    check("traversal caption: uses the document",
          tbox_scanmap_entry(126,
                             "{\"v\":1,\"type\":\"file\",\"name\":\"../x\","
                             "\"sha256\":\"aa\",\"mtime\":1}",
                             "real.txt", 12, 5, 0, &e), 0);
    check_str("traversal caption: named after the document", e.name,
              "real.txt");

    /* and when the document is unusable too, the message id */
    check("untrusted caption with no name: message-<id>",
          (tbox_scanmap_entry(127,
                              "{\"v\":2,\"type\":\"file\",\"name\":\"x.txt\"}",
                              NULL, 12, 5, 0, &e), 0), 0);
    check_str("untrusted caption with no name: message-<id>", e.name,
              "message-127");
}

/* ---- misuse ----------------------------------------------------------------- */

static void test_edges(void)
{
    tbox_archive_entry_t e;

    check("a NULL out is refused", tbox_scanmap_entry(1, NULL, NULL, 0, 0, 0, NULL),
          -1);

    /* an out struct is fully written, never merged with the caller's junk */
    memset(&e, 0xAA, sizeof e);
    check("out is overwritten: returns 0",
          tbox_scanmap_entry(131, NULL, "a.txt", 1, 2, 3, &e), 0);
    check_str("out is overwritten: name", e.name, "a.txt");
    check("out is overwritten: is_tbox", e.is_tbox, 0);
    check("out is overwritten: sha256", e.sha256[0], 0);
    check("out is overwritten: no stale bytes", e.sha256[64], 0);

    /* the same for a caption: a stale kind cannot survive */
    memset(&e, 0xAA, sizeof e);
    check("caption out is overwritten",
          tbox_scanmap_entry(132, DIR_CAP, "m.txt", 1, 5, 6, &e), 0);
    check("caption out: is_dir is 1", e.is_dir, 1);
    check("caption out: size is 0", e.size, 0);

    /* a zero message id still yields a usable name */
    check("zero message id: falls back cleanly",
          (tbox_scanmap_entry(0, NULL, NULL, 0, 0, 0, &e), 0), 0);
    check_str("zero message id: message-0", e.name, "message-0");
}

/* ---- the whole scan, end to end -------------------------------------------- */

/* The sequence a real scan produces, folded into one archive: the tree the
 * FTP server then serves must match what v0.3 served. */
static void test_scenario(void)
{
    tbox_archive_entry_t e;
    tbox_archive_t ar;
    tbox_archive_entry_t *out = NULL;
    size_t cap = 0;
    int n;

    tbox_archive_init(&ar);

    tbox_scanmap_entry(201, FILE_CAP, "up.bin", 11, 1, 61, &e);
    check("scenario: file added", tbox_archive_add(&ar, &e), 0);

    tbox_scanmap_entry(202, DIR_CAP, "marker.txt", 1, 1700000202, 62, &e);
    check("scenario: dir added", tbox_archive_add(&ar, &e), 0);

    tbox_scanmap_entry(203, "holiday photo", "IMG_0042.JPG", 300000,
                       1700000203, 63, &e);
    check("scenario: foreign photo added", tbox_archive_add(&ar, &e), 0);

    tbox_scanmap_entry(204, NULL, NULL, 0, 1700000204, 0, &e);
    check("scenario: unnamed document added", tbox_archive_add(&ar, &e), 0);

    /* the same photo again, newer and bigger: newest wins */
    tbox_scanmap_entry(205, "holiday photo", "IMG_0042.JPG", 400000,
                       1700000205, 64, &e);
    check("scenario: re-upload added", tbox_archive_add(&ar, &e), 0);

    check("scenario: total entries", (int)ar.count, 4);

    /* implied dir "a" for a/b.txt */
    tbox_archive_materialize(&ar);
    check("scenario: implied dir synthesized", (int)ar.count, 5);

    /* list() reports an error code and fills *cap; 0 here means "no error". */
    check("scenario: root lists", tbox_archive_list(&ar, "/", &out, &cap), 0);
    n = (int)cap;
    if (n == 5) {
        check_str("scenario: root [0]", out[0].name, "IMG_0042.JPG");
        check_str("scenario: root [1]", out[1].name, "a");
        check_str("scenario: root [2]", out[2].name, "message-204");
        check_str("scenario: root [3]", out[3].name, "photos");
        check("scenario: photo size is the newer one", (long long)out[0].size,
              400000);
        check("scenario: photo is_tbox is 0", out[0].is_tbox, 0);
        check("scenario: implied dir has no message", out[1].message_id,
              TBOX_MSG_NONE);
        check("scenario: message-204 is a file", out[2].is_dir, 0);
        check("scenario: message-204 is dated", out[2].mtime, 1700000204);
        check("scenario: photos is a dir", out[3].is_dir, 1);
    }
    free(out);

    out = NULL;
    cap = 0;
    check("scenario: a/ lists", tbox_archive_list(&ar, "/a", &out, &cap), 0);
    n = (int)cap;
    if (n == 1) {
        check_str("scenario: a/b.txt", out[0].name, "a/b.txt");
        check("scenario: a/b.txt is a file", out[0].is_dir, 0);
        check("scenario: a/b.txt is ours", out[0].is_tbox, 1);
        check("scenario: a/b.txt size", out[0].size, 11);
    }
    free(out);

    tbox_archive_free(&ar);
}

int main(void)
{
    test_foreign();
    test_caption_file();
    test_caption_dir();
    test_untrusted_captions();
    test_edges();
    test_scenario();

    printf(failures ? "FAILED (%d)\n" : "test_scanmap: all green\n", failures);
    return failures ? 1 : 0;
}
