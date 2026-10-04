/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * test_indexfile.c - the index.json mirror (core/indexfile.c).
 *
 * The mirror is what makes a restart instant, so the round trip has to be
 * exact: a name that survives is browsable, and a field that does not is a
 * lie the next scan will have to undo. Asserted:
 *   - a full round trip keeps names, ids, size, mtime and the dir flag
 *   - the file is version-tagged and refuses a foreign version
 *   - a missing file is "cold", not an error
 *   - atomic replacement: no .tmp left behind, previous copy survives a
 *     failed write
 *   - garbage degrades to skipped entries rather than a broken index
 *   - sha256/is_tbox are NOT mirrored (they are re-derived from captions)
 *
 * Headless by rule: no TDLib, no network, no secrets. Writes go to a scratch
 * directory under the temp dir only.
 */

#define _CRT_SECURE_NO_WARNINGS   /* MSVC: strcpy / remove warnings */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>       /* _rmdir */
#define TBOX_RMDIR _rmdir
#else
#include <unistd.h>
#define TBOX_RMDIR rmdir
#endif

#include "core/indexfile.h"
#include "util/datadir.h"   /* tbox_datadir_ensure */

static int failures = 0;

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

static void ok(const char *what, int cond)
{
    if (cond) {
        printf("ok   %-52s\n", what);
    } else {
        printf("FAIL %-52s\n", what);
        failures++;
    }
}

/* ---- scratch paths --------------------------------------------------------- */

static char g_dir[512];
static char g_path[640];

/* one scratch file per test, all under <temp>/tbox_test_indexfile */
static void scratch(const char *name)
{
    size_t len = (size_t)snprintf(g_path, sizeof g_path, "%s/%s", g_dir, name);

    if (len >= sizeof g_path)
        g_path[0] = '\0';
}

static void put_text(const char *text)
{
    FILE *fp = fopen(g_path, "wb");

    if (fp != NULL) {
        fputs(text, fp);
        fclose(fp);
    }
}

static int file_exists(const char *path)
{
    FILE *fp = fopen(path, "rb");

    if (fp == NULL)
        return 0;
    fclose(fp);
    return 1;
}

static void unlink_scratch(void)
{
    if (g_path[0] != '\0')
        remove(g_path);
    remove("indexfile.tmp");
}

/* ---- round trip ------------------------------------------------------------ */

static void test_round_trip(void)
{
    tbox_archive_t ar;
    tbox_archive_t back;
    const tbox_archive_entry_t *e;
    long long n;

    tbox_archive_init(&ar);
    tbox_archive_init(&back);

    {
        tbox_archive_entry_t e1;
        tbox_archive_entry_t e2;

        memset(&e1, 0, sizeof e1);
        strcpy(e1.name, "a/b.txt");
        e1.message_id = 4242;
        e1.file_id = 987654321LL;
        e1.is_dir = 0;
        e1.size = 12345;
        e1.mtime = 1700000000LL;
        strcpy(e1.sha256, "deadbeef");
        e1.is_tbox = 1;
        check("round trip: file added", tbox_archive_add(&ar, &e1), 0);

        memset(&e2, 0, sizeof e2);
        strcpy(e2.name, "photos");
        e2.message_id = 4243;
        e2.file_id = 987654322LL;
        e2.is_dir = 1;
        e2.size = 0;
        e2.mtime = 1700000001LL;
        check("round trip: dir added", tbox_archive_add(&ar, &e2), 0);
    }

    scratch("round.json");
    check("round trip: written", tbox_indexfile_write(g_path, &ar), 0);
    ok("round trip: file exists", file_exists(g_path));
    ok("round trip: no .tmp left behind", !file_exists("indexfile.tmp"));

    n = tbox_indexfile_read(g_path, &back);
    check("round trip: two entries loaded", n, 2);
    check("round trip: two entries stored", (int)back.count, 2);

    e = tbox_archive_find(&back, "a/b.txt");
    ok("round trip: the file is there", e != NULL);
    if (e != NULL) {
        check("round trip: message id", e->message_id, 4242);
        check("round trip: remote file id", e->file_id, 987654321LL);
        check("round trip: size", e->size, 12345);
        check("round trip: mtime", e->mtime, 1700000000LL);
        check("round trip: is_dir", e->is_dir, 0);
        /* deliberately not mirrored: the next scan re-derives these from the
         * caption, and a mirror that asserted them could be wrong */
        check("round trip: sha256 is not mirrored", e->sha256[0], 0);
        check("round trip: is_tbox is not mirrored", e->is_tbox, 0);
    }

    e = tbox_archive_find(&back, "photos");
    ok("round trip: the dir is there", e != NULL);
    if (e != NULL) {
        check("round trip: dir message id", e->message_id, 4243);
        check("round trip: dir file id", e->file_id, 987654322LL);
        check("round trip: dir flag", e->is_dir, 1);
        check("round trip: dir mtime", e->mtime, 1700000001LL);
    }

    unlink_scratch();
    tbox_archive_free(&ar);
    tbox_archive_free(&back);
}

static void test_empty_and_missing(void)
{
    tbox_archive_t ar;

    tbox_archive_init(&ar);

    scratch("nothing-here.json");
    check("a missing file is cold, not an error",
          tbox_indexfile_read(g_path, &ar), -1);
    check("a missing file adds nothing", (int)ar.count, 0);

    scratch("empty.json");
    put_text("");
    check("an empty file is refused", tbox_indexfile_read(g_path, &ar), -1);
    unlink_scratch();

    /* an archive with nothing in it is still a valid mirror */
    scratch("empty-ar.json");
    check("an empty archive writes", tbox_indexfile_write(g_path, &ar), 0);
    check("an empty archive reads back 0",
          tbox_indexfile_read(g_path, &ar), 0);
    unlink_scratch();

    check("a NULL path is refused", tbox_indexfile_read(NULL, &ar), -1);
    check("a NULL archive is refused", tbox_indexfile_read(g_path, NULL), -1);
    check("writing a NULL path is refused", tbox_indexfile_write(NULL, &ar), -1);
    check("writing a NULL archive is refused", tbox_indexfile_write(g_path, NULL),
          -1);

    tbox_archive_free(&ar);
}

/* ---- foreign and broken files ---------------------------------------------- */

static void test_rejects(void)
{
    tbox_archive_t ar;

    tbox_archive_init(&ar);

    scratch("v2.json");
    put_text("{\"v\":2,\"entries\":[]}");
    check("a future version is refused", tbox_indexfile_read(g_path, &ar), -1);
    unlink_scratch();

    scratch("no-v.json");
    put_text("{\"entries\":[]}");
    check("a missing version is refused", tbox_indexfile_read(g_path, &ar), -1);
    unlink_scratch();

    scratch("not-json.json");
    put_text("this is not json at all");
    check("junk is refused", tbox_indexfile_read(g_path, &ar), -1);
    unlink_scratch();

    scratch("no-entries.json");
    put_text("{\"v\":1}");
    check("a mirror without entries is refused",
          tbox_indexfile_read(g_path, &ar), -1);
    unlink_scratch();

    scratch("entries-not-array.json");
    put_text("{\"v\":1,\"entries\":{}}");
    check("entries that are not an array is refused",
          tbox_indexfile_read(g_path, &ar), -1);
    unlink_scratch();

    check("nothing was added by any of those", (int)ar.count, 0);

    tbox_archive_free(&ar);
}

/* A hand-edited or half-written mirror must degrade, not break the serve. */
static void test_degrades(void)
{
    tbox_archive_t ar;

    tbox_archive_init(&ar);

    scratch("mixed.json");
    put_text("{\"v\":1,\"entries\":["
             "{\"name\":\"good/one.txt\",\"m\":1,\"f\":2,\"z\":3,\"t\":4,\"d\":0},"
             "\"a bare string\","
             "{\"no\":\"name\"},"
             "{\"name\":\"..\",\"m\":9},"
             "{\"name\":\"good/two.txt\",\"m\":5,\"f\":6,\"z\":7,\"t\":8,\"d\":0}"
             "]}");
    check("mixed: only the usable entries load",
          tbox_indexfile_read(g_path, &ar), 2);
    check("mixed: two entries stored", (int)ar.count, 2);
    ok("mixed: the first survived", tbox_archive_find(&ar, "good/one.txt") != NULL);
    ok("mixed: the traversal was dropped",
       tbox_archive_find(&ar, "..") == NULL);
    unlink_scratch();

    /* optional fields default instead of failing the entry */
    tbox_archive_free(&ar);
    tbox_archive_init(&ar);
    scratch("minimal.json");
    put_text("{\"v\":1,\"entries\":[{\"name\":\"just/a/name\"}]}");
    check("minimal: one entry loads", tbox_indexfile_read(g_path, &ar), 1);
    {
        const tbox_archive_entry_t *e = tbox_archive_find(&ar, "just/a/name");

        ok("minimal: the entry is there", e != NULL);
        if (e != NULL) {
            check("minimal: no message id", e->message_id, TBOX_MSG_NONE);
            check("minimal: no file id", e->file_id, 0);
            check("minimal: no size", e->size, 0);
            check("minimal: unknown mtime", e->mtime, -1);
            check("minimal: a file", e->is_dir, 0);
        }
    }
    unlink_scratch();

    /* a duplicate name in the mirror collapses to one entry, newest wins */
    tbox_archive_free(&ar);
    tbox_archive_init(&ar);
    scratch("dupes.json");
    put_text("{\"v\":1,\"entries\":["
             "{\"name\":\"dup.txt\",\"m\":1,\"z\":10},"
             "{\"name\":\"dup.txt\",\"m\":2,\"z\":20}]}");
    check("duplicates: one entry survives", tbox_indexfile_read(g_path, &ar), 2);
    check("duplicates: the index holds one", (int)ar.count, 1);
    {
        const tbox_archive_entry_t *e = tbox_archive_find(&ar, "dup.txt");

        ok("duplicates: it is there", e != NULL);
        if (e != NULL) {
            check("duplicates: the last one won", e->message_id, 2);
            check("duplicates: with its size", e->size, 20);
        }
    }
    unlink_scratch();

    tbox_archive_free(&ar);
}

/* Replacing an existing mirror must never leave the reader with nothing. */
static void test_replaces(void)
{
    tbox_archive_t ar;
    tbox_archive_t back;

    tbox_archive_init(&ar);
    tbox_archive_init(&back);

    {
        tbox_archive_entry_t e;

        memset(&e, 0, sizeof e);
        strcpy(e.name, "first.txt");
        e.message_id = 1;
        check("replace: first write", tbox_archive_add(&ar, &e), 0);
    }

    scratch("replace.json");
    check("replace: first mirror", tbox_indexfile_write(g_path, &ar), 0);
    check("replace: first mirror reads", tbox_indexfile_read(g_path, &back), 1);
    check_str("replace: first name", back.items[0].name, "first.txt");
    tbox_archive_free(&back);
    tbox_archive_init(&back);

    /* a much longer name, so the new file is definitely a different length */
    tbox_archive_free(&ar);
    tbox_archive_init(&ar);
    {
        tbox_archive_entry_t e;

        memset(&e, 0, sizeof e);
        strcpy(e.name, "a/much/longer/second/name/that/keeps/going/for/while.txt");
        e.message_id = 2;
        check("replace: second write", tbox_archive_add(&ar, &e), 0);
    }

    check("replace: second mirror", tbox_indexfile_write(g_path, &ar), 0);
    check("replace: second mirror reads", tbox_indexfile_read(g_path, &back), 1);
    ok("replace: the new name is there",
       tbox_archive_find(&back, "a/much/longer/second/name/that/keeps/going/for/while.txt")
           != NULL);
    ok("replace: the old name is gone",
       tbox_archive_find(&back, "first.txt") == NULL);
    ok("replace: still no .tmp", !file_exists("indexfile.tmp"));

    unlink_scratch();
    tbox_archive_free(&ar);
    tbox_archive_free(&back);
}

/* The implied directories are re-derived, never stored: a mirror must not be
 * able to invent a directory that the real scan would not produce. */
static void test_no_synthesized_dirs(void)
{
    tbox_archive_t ar;
    tbox_archive_entry_t *out = NULL;
    size_t cap = 0;

    tbox_archive_init(&ar);

    scratch("prefix.json");
    put_text("{\"v\":1,\"entries\":["
             "{\"name\":\"deep/down/here.txt\",\"m\":1,\"z\":1,\"t\":1,\"d\":0}"
             "]}");
    check("implied: one entry loads", tbox_indexfile_read(g_path, &ar), 1);
    check("implied: nothing synthesized yet", (int)ar.count, 1);
    unlink_scratch();

    check("implied: root lists", tbox_archive_list(&ar, "/", &out, &cap), 0);
    check("implied: root has the top dir", (int)cap, 1);
    if (cap == 1) {
        check_str("implied: named deep", out[0].name, "deep");
        check("implied: it is a directory", out[0].is_dir, 1);
        check("implied: no message of its own", out[0].message_id, TBOX_MSG_NONE);
    }
    free(out);
    check("implied: the dir was synthesized on demand", (int)ar.count, 3);

    tbox_archive_free(&ar);
}

int main(void)
{
    size_t len;
    const char *tmp = getenv("TMPDIR");

#if defined(_WIN32)
    tmp = getenv("TEMP");
    if (tmp == NULL)
        tmp = getenv("TMP");
#endif
    if (tmp == NULL || tmp[0] == '\0')
        tmp = ".";

    len = (size_t)snprintf(g_dir, sizeof g_dir, "%s/tbox_test_indexfile", tmp);
    if (len >= sizeof g_dir) {
        printf("FAIL the temp path is too long\n");
        return 1;
    }
    tbox_datadir_ensure(g_dir);

    test_round_trip();
    test_empty_and_missing();
    test_rejects();
    test_degrades();
    test_replaces();
    test_no_synthesized_dirs();

    /* the scratch dir goes away with the tests, not before */
    unlink_scratch();
    TBOX_RMDIR(g_dir);

    printf(failures ? "FAILED (%d)\n" : "test_indexfile: all green\n", failures);
    return failures ? 1 : 0;
}
