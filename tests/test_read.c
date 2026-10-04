/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * tests/test_read.c - the file cache (src/core/read.c).
 *
 * Headless by rule: no TDLib, no network, no secrets, and never the real data
 * directory - every path is a scratch directory under TEMP/tmp, removed again
 * at the end.
 *
 * The fetcher is injected, so this exercises the real cache logic (part files,
 * atomic replace, LRU eviction, read/seek) without Telegram.
 */

#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#include "core/read.h"
#include "util/datadir.h"

static int checks;
static int failures;

static void ok(int cond, const char *what)
{
    checks++;
    if (cond) {
        printf("ok   %s\n", what);
    } else {
        failures++;
        printf("FAIL %s\n", what);
    }
    fflush(stdout);      /* a crash must not swallow the progress so far */
}

/* ---- scratch directory --------------------------------------------------- */

static char g_root[512];
static char g_cache[640];

static int file_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

static int dir_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && (st.st_mode & S_IFDIR) != 0;
}

static void dir_remove(const char *path)
{
#ifdef _WIN32
    _rmdir(path);
#else
    rmdir(path);
#endif
}

/* Delete every file directly inside `dir` (no recursion needed here). */
static void empty_dir(const char *dir)
{
#ifdef _WIN32
    char pattern[700];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    if (tbox_datadir_join(pattern, sizeof pattern, dir, "*") != 0)
        return;
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        char victim[800];

        if (tbox_datadir_join(victim, sizeof victim, dir, fd.cFileName) == 0
            && strcmp(fd.cFileName, ".") != 0
            && strcmp(fd.cFileName, "..") != 0)
            remove(victim);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);

    if (d == NULL)
        return;
    for (;;) {
        struct dirent *de = readdir(d);
        char victim[800];

        if (de == NULL)
            break;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (tbox_datadir_join(victim, sizeof victim, dir, de->d_name) == 0)
            remove(victim);
    }
    closedir(d);
#endif
}

static void drop_scratch(void)
{
    if (g_cache[0] != '\0')
        empty_dir(g_cache);
    if (g_cache[0] != '\0')
        dir_remove(g_cache);
    if (g_root[0] != '\0')
        dir_remove(g_root);
}

static void make_scratch(void)
{
    const char *tmp = getenv("TEMP");

    if (tmp == NULL || tmp[0] == '\0')
        tmp = getenv("TMPDIR");
    if (tmp == NULL || tmp[0] == '\0')
        tmp = ".";

    g_root[0] = '\0';
    g_cache[0] = '\0';

    if (tbox_datadir_join(g_root, sizeof g_root, tmp, "tbox_test_read") != 0
        || tbox_datadir_cache(g_cache, sizeof g_cache, g_root) != 0) {
        printf("FAIL scratch path too long\n");
        failures++;
        return;
    }

    drop_scratch();
    ok(tbox_datadir_ensure(g_root) == 0, "scratch dir created");
}

/* Each test starts from an empty cache so byte counts are unambiguous. */
static void reset_cache(void)
{
    empty_dir(g_cache);
}

/* Build <cache>/<id>.bin without ever using snprintf("%s/%s"). */
static int body_path(char *out, size_t size, long long id)
{
    char leaf[64];
    int n = snprintf(leaf, sizeof leaf, "%lld.bin", id);

    if (n < 0 || (size_t)n >= sizeof leaf)
        return -1;
    return tbox_datadir_join(out, size, g_cache, leaf);
}

/* ---- fake fetcher -------------------------------------------------------- */

typedef struct
{
    int calls;        /* how many times the fetcher ran */
    int fail_id;      /* return -1 for this id */
    int cancel_id;    /* return 1 (cancelled) for this id */
    long long size;   /* bytes to write */
    int no_write;     /* return 0 without creating the file */

} fake_t;

static fake_t g_fake;

static int fake_fetch(void *user, long long file_id, const char *dest)
{
    FILE *f;
    long long i;

    (void)user;
    g_fake.calls++;

    if (file_id == g_fake.fail_id)
        return -1;
    if (file_id == g_fake.cancel_id)
        return 1;
    if (g_fake.no_write)
        return 0;

    f = fopen(dest, "wb");
    if (f == NULL)
        return -1;
    for (i = 0; i < g_fake.size; i++) {         /* byte i == (char)(i % 251) */
        if (fputc((int)(i % 251), f) == EOF) {
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return 0;
}

/* ---- tests --------------------------------------------------------------- */

static void test_open_creates_dir(void)
{
    tbox_cache_t cache;

    reset_cache();
    memset(&g_fake, 0, sizeof g_fake);
    ok(tbox_cache_open(&cache, g_root, 1024, fake_fetch, NULL) == 0,
       "cache opens on a fresh data root");
    ok(dir_exists(g_cache), "the cache directory was created");
    ok(tbox_cache_bytes(&cache) == 0, "a fresh cache measures zero bytes");
    tbox_cache_close(&cache);
}

/* Fetch on first open, read sequentially, seek back, reuse on second open. */
static void test_fetch_and_read(void)
{
    tbox_cache_t cache;
    tbox_cfile_t f;
    char buf[16];
    char path[800];
    long long got;

    reset_cache();
    memset(&g_fake, 0, sizeof g_fake);
    g_fake.size = 40;

    ok(tbox_cache_open(&cache, g_root, 4096, fake_fetch, NULL) == 0,
       "cache opens for the read test");

    ok(tbox_cfile_open(&cache, 1, &f) == 0, "first open fetches the body");
    ok(g_fake.calls == 1, "the fetcher ran exactly once");
    ok(tbox_cfile_size(&f) == 40, "reported size matches what was fetched");

    memset(buf, 0, sizeof buf);
    got = tbox_cfile_read(&f, buf, 10);
    ok(got == 10, "read returns the requested chunk");
    ok(buf[0] == 0 && buf[9] == 9, "chunk content is the fake body");

    ok(tbox_cfile_seek(&f, 0) == 0, "seek back to the start");
    memset(buf, 0, sizeof buf);
    got = tbox_cfile_read(&f, buf, 10);
    ok(got == 10 && buf[0] == 0, "seek really moved the read position");

    got = tbox_cfile_read(&f, buf, 10);
    ok(got == 10, "second chunk");
    got = tbox_cfile_read(&f, buf, 10);
    ok(got == 10, "third chunk");
    got = tbox_cfile_read(&f, buf, 10);
    ok(got == 10, "fourth chunk fills the body");
    got = tbox_cfile_read(&f, buf, 10);
    ok(got == 0, "read at end of file returns 0, not an error");

    ok(tbox_cfile_seek(&f, 41) == -1, "seek past the end is refused");
    ok(tbox_cfile_seek(&f, -1) == -1, "negative seek is refused");
    ok(tbox_cfile_seek(&f, 20) == 0, "seek to the middle is allowed");
    memset(buf, 0, sizeof buf);
    got = tbox_cfile_read(&f, buf, 4);
    ok(got == 4 && buf[0] == 20, "read after a mid-file seek is correct");

    tbox_cfile_close(&f);

    if (body_path(path, sizeof path, 1) == 0)
        ok(file_exists(path), "the body is cached as <file_id>.bin");

    ok(tbox_cache_bytes(&cache) == 40, "the cache accounts for the new body");

    ok(tbox_cfile_open(&cache, 1, &f) == 0, "second open reuses the body");
    ok(g_fake.calls == 1, "the fetcher did not run twice");
    ok(tbox_cfile_size(&f) == 40, "the reused body has the same size");
    tbox_cfile_close(&f);

    tbox_cache_close(&cache);
}

static void test_no_part_leftovers(void)
{
    tbox_cache_t cache;
    tbox_cfile_t f;
    char leaf[64];
    char path[800];
    int n;

    reset_cache();
    memset(&g_fake, 0, sizeof g_fake);
    g_fake.size = 8;

    ok(tbox_cache_open(&cache, g_root, 4096, fake_fetch, NULL) == 0,
       "cache opens for the part-file test");
    ok(tbox_cfile_open(&cache, 6, &f) == 0, "fetch body 6");
    tbox_cfile_close(&f);

    n = snprintf(leaf, sizeof leaf, ".part-6-%d", 0);
    if (n > 0 && (size_t)n < sizeof leaf
        && tbox_datadir_join(path, sizeof path, g_cache, leaf) == 0) {
        int found = 0;
        int counter;

        /* any .part-6-* left behind would be a bug: check the first few */
        for (counter = 0; counter < 4; counter++) {
            char name2[64];
            char p2[800];

            n = snprintf(name2, sizeof name2, ".part-6-%d", counter);
            if (n <= 0 || (size_t)n >= sizeof name2)
                break;
            if (tbox_datadir_join(p2, sizeof p2, g_cache, name2) == 0
                && file_exists(p2))
                found++;
        }
        ok(found == 0, "no .part file is left behind after a rename");
    }

    tbox_cache_close(&cache);
}

static void test_fetch_failure(void)
{
    tbox_cache_t cache;
    tbox_cfile_t f;
    char path[800];

    reset_cache();
    memset(&g_fake, 0, sizeof g_fake);
    g_fake.size = 10;
    g_fake.fail_id = 2;

    ok(tbox_cache_open(&cache, g_root, 4096, fake_fetch, NULL) == 0,
       "cache opens for the failure test");
    ok(tbox_cfile_open(&cache, 2, &f) == -1, "a failed fetch is an error");
    ok(tbox_cache_bytes(&cache) == 0, "a failed fetch adds no bytes");
    if (body_path(path, sizeof path, 2) == 0)
        ok(file_exists(path) == 0, "a failed fetch leaves no body");

    g_fake.fail_id = 0;
    g_fake.cancel_id = 2;
    ok(tbox_cfile_open(&cache, 2, &f) == -2, "a cancelled fetch is -2");
    if (body_path(path, sizeof path, 2) == 0)
        ok(file_exists(path) == 0, "a cancelled fetch leaves no body");
    g_fake.cancel_id = 0;

    /* a fetcher that claims success but writes nothing must not be trusted */
    g_fake.no_write = 1;
    ok(tbox_cfile_open(&cache, 8, &f) == -1,
       "a fetch that produced no file is an error");
    g_fake.no_write = 0;

    tbox_cache_close(&cache);
}

static void test_no_fetcher(void)
{
    tbox_cache_t cache;
    tbox_cfile_t f;

    memset(&g_fake, 0, sizeof g_fake);
    ok(tbox_cache_open(&cache, g_root, 4096, NULL, NULL) == 0,
       "cache opens without a fetcher (names-only server)");
    ok(tbox_cfile_open(&cache, 3, &f) == -1,
       "no fetcher means an explicit error, not a silent hang");
    ok(tbox_cfile_open(&cache, 0, &f) == -1,
       "file id 0 is refused before any I/O");
    tbox_cache_close(&cache);
}

/*
 * LRU: with a 2000-byte budget and 1000-byte bodies, the third fetch must
 * evict the oldest body and leave exactly two on disk.
 *
 * The victim is deterministic on both platforms: NTFS mtimes are 100ns apart
 * so the first fetch is oldest, and POSIX second-resolution mtimes tie, where
 * cmp_oldest falls back to the name - and "4.bin" sorts first there too.
 */
static void test_eviction(void)
{
    tbox_cache_t cache;
    tbox_cfile_t f;
    char path[800];

    reset_cache();
    memset(&g_fake, 0, sizeof g_fake);
    g_fake.size = 1000;

    ok(tbox_cache_open(&cache, g_root, 2000, fake_fetch, NULL) == 0,
       "cache opens with a 2000-byte budget");

    ok(tbox_cfile_open(&cache, 4, &f) == 0, "fetch body 4");
    tbox_cfile_close(&f);
    ok(tbox_cache_bytes(&cache) == 1000, "one body cached");

    ok(tbox_cfile_open(&cache, 5, &f) == 0, "fetch body 5");
    tbox_cfile_close(&f);
    ok(tbox_cache_bytes(&cache) == 2000, "two bodies fit the budget");

    /* simulate a restart: a fresh cache must measure what is already there */
    ok(tbox_cache_open(&cache, g_root, 2000, fake_fetch, NULL) == 0,
       "reopen to simulate a restart");
    ok(tbox_cache_bytes(&cache) == 2000,
       "a restart measures the existing bodies instead of re-downloading");

    ok(tbox_cfile_open(&cache, 6, &f) == 0, "fetch body 6");
    tbox_cfile_close(&f);
    ok(tbox_cache_bytes(&cache) == 2000, "the oldest body was evicted");
    ok(g_fake.calls == 3, "the third fetch actually ran");

    if (body_path(path, sizeof path, 6) == 0)
        ok(file_exists(path), "the freshly fetched body survives");
    if (body_path(path, sizeof path, 5) == 0)
        ok(file_exists(path), "the newest older body survives");
    if (body_path(path, sizeof path, 4) == 0)
        ok(file_exists(path) == 0, "the oldest body was the one evicted");

    /* a body that is still cached is reopened, not refetched */
    g_fake.calls = 0;
    ok(tbox_cfile_open(&cache, 6, &f) == 0, "a surviving body reopens");
    ok(g_fake.calls == 0, "and is not fetched again");
    tbox_cfile_close(&f);

    tbox_cache_close(&cache);
}

/* An over-budget cache from a previous run is trimmed at open time. */
static void test_trim_on_open(void)
{
    tbox_cache_t cache;
    char path[800];
    int kept = 0;
    int i;

    /* three 1000-byte bodies, then a much smaller budget */
    reset_cache();
    memset(&g_fake, 0, sizeof g_fake);
    g_fake.size = 1000;
    ok(tbox_cache_open(&cache, g_root, 8000, fake_fetch, NULL) == 0,
       "cache opens with a generous budget");
    for (i = 0; i < 3; i++) {
        tbox_cfile_t cf;

        if (tbox_cfile_open(&cache, 20 + i, &cf) == 0)
            tbox_cfile_close(&cf);
    }
    ok(tbox_cache_bytes(&cache) == 3000, "three bodies cached");

    /* a different process shrank the limit: reopen at 1000 bytes */
    ok(tbox_cache_open(&cache, g_root, 1000, fake_fetch, NULL) == 0,
       "reopen with a smaller budget");
    ok(tbox_cache_bytes(&cache) <= 1000, "an over-budget cache is trimmed");

    /* the directory and the counter must agree */
    for (i = 0; i < 3; i++) {
        if (body_path(path, sizeof path, 20 + i) == 0 && file_exists(path))
            kept++;
    }
    ok(kept == 1, "exactly one body survived a 1000-byte budget");

    tbox_cache_close(&cache);
}

int main(void)
{
    printf("== test_read ==\n");

    make_scratch();
    test_open_creates_dir();
    test_fetch_and_read();
    test_no_part_leftovers();
    test_fetch_failure();
    test_no_fetcher();
    test_eviction();
    test_trim_on_open();

    drop_scratch();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}