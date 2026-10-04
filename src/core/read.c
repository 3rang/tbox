/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * core/read.c - the bounded local cache of archive file bodies.
 * No TDLib, no Telegram knowledge: how bytes arrive is the injected
 * tbox_cache_fetch_fn, which is what makes this layer testable headlessly.
 *
 *   <data>/cache/<remote file id>.bin        the body
 *   <data>/cache/.part-<id>-<n>              in-flight fetch, renamed into place
 *
 * Every function here is safe to call from several transport threads at once:
 * the only shared mutable state is the cache struct itself, and a body that
 * two threads fetch simultaneously is still correct - the slower rename loses,
 * finds the body already present, and opens that one.
 */

#define _CRT_SECURE_NO_WARNINGS   /* MSVC: stat() has a POSIX-ish spelling */

#include "read.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "util/datadir.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

/* ".part-<20 digits>-<counter>" fits well inside this. */
#define CFILE_MAX 64

/* Far above any real cache directory; a guard, not a budget. */
#define CACHE_MAX_ENTRIES 4096

/* One cached body file. */
typedef struct
{
    char name[CFILE_MAX];
    long long size;
    long long mtime;              /* unix seconds; 0 when unknown */

} cbody_t;

/* Path buffers are the data dir plus a leaf. */
#define PATHBUF 640

static int g_part_counter = 0;    /* makes each in-flight fetch unique */

/* ---- small filesystem helpers -------------------------------------------- */

static int file_exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

/* Either out parameter may be NULL when the caller only wants one of them. */
static int stat_body(const char *path, long long *size, long long *mtime)
{
    struct stat st;

    if (stat(path, &st) != 0)
        return -1;
    if (size != NULL)
        *size = (long long)st.st_size;
    if (mtime != NULL)
        *mtime = (long long)st.st_mtime;
    return 0;
}

/* ---- cache paths (length-checked, never snprintf("%s/%s")) ---------------- */

static int body_leaf(char *leaf, size_t lsize, long long file_id)
{
    int n = snprintf(leaf, lsize, "%lld.bin", file_id);

    return (n < 0 || (size_t)n >= lsize) ? -1 : 0;
}

static int body_path(char *out, size_t size, const tbox_cache_t *cache,
                     long long file_id)
{
    char leaf[CFILE_MAX];

    if (body_leaf(leaf, sizeof leaf, file_id) != 0)
        return -1;
    return tbox_datadir_join(out, size, cache->root, leaf);
}

static int part_path(char *out, size_t size, const tbox_cache_t *cache,
                     long long file_id)
{
    char leaf[CFILE_MAX];
    int n = snprintf(leaf, sizeof leaf, ".part-%lld-%d", file_id,
                     g_part_counter++);

    if (n < 0 || (size_t)n >= sizeof leaf)
        return -1;
    return tbox_datadir_join(out, size, cache->root, leaf);
}

/* ---- scanning the cache directory ----------------------------------------- */

/*
 * Fill `list` with the cached bodies, newest last. Returns the number found,
 * or -1 on a hard error (out of memory). A missing directory simply means
 * "nothing cached yet".
 */
static int scan_cache(const tbox_cache_t *cache, cbody_t *list, int max)
{
    int count = 0;

    if (max > CACHE_MAX_ENTRIES)
        max = CACHE_MAX_ENTRIES;

#ifdef _WIN32
    {
        char pattern[PATHBUF];
        WIN32_FIND_DATAA fd;
        HANDLE h;

        if (tbox_datadir_join(pattern, sizeof pattern, cache->root,
                              "*.bin") != 0)
            return 0;

        h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE)
            return 0;                       /* empty or missing */

        do {
            size_t len = strlen(fd.cFileName);

            if (count >= max)
                break;
            if (len == 0 || len >= CFILE_MAX)
                continue;
            if (strcmp(fd.cFileName, ".") == 0
                || strcmp(fd.cFileName, "..") == 0)
                continue;

            memcpy(list[count].name, fd.cFileName, len + 1);
            count++;
        } while (FindNextFileA(h, &fd));

        FindClose(h);
    }
#else
    {
        DIR *dir = opendir(cache->root);

        if (dir == NULL)
            return 0;                       /* empty or missing */

        for (;;) {
            struct dirent *de = readdir(dir);
            size_t len;

            if (de == NULL || count >= max)
                break;
            len = strlen(de->d_name);
            if (len < 5 || len >= CFILE_MAX)   /* at least "x.bin" */
                continue;
            if (strcmp(de->d_name + len - 4, ".bin") != 0)
                continue;

            memcpy(list[count].name, de->d_name, len + 1);
            count++;
        }

        closedir(dir);
    }
#endif

    /* One stat per name, on both platforms: the directory scan only found
     * names, and a single code path is worth more than saving a syscall. */
    {
        int i;

        for (i = 0; i < count; i++) {
            char path[PATHBUF];

            list[i].size = 0;
            list[i].mtime = 0;
            if (tbox_datadir_join(path, sizeof path, cache->root,
                                  list[i].name) == 0)
                (void)stat_body(path, &list[i].size, &list[i].mtime);
        }
    }

    return count;
}

/* ---- eviction ------------------------------------------------------------ */

static int cmp_oldest(const void *a, const void *b)
{
    const cbody_t *x = (const cbody_t *)a;
    const cbody_t *y = (const cbody_t *)b;

    if (x->mtime < y->mtime)
        return -1;
    if (x->mtime > y->mtime)
        return 1;
    return strcmp(x->name, y->name);
}

/*
 * Delete the oldest cached bodies until the directory fits max_bytes. `keep`
 * is a body name that must survive (the one we are about to open).
 *
 * A body another thread currently has open cannot be deleted on Windows; that
 * is accepted - this budgets space, it does not guarantee a number.
 */
/* Re-measure the directory. Only needed on the rare races and after a crash. */
static long long remeasure(tbox_cache_t *cache)
{
    cbody_t *list;
    long long total = 0;
    int count;
    int i;

    list = (cbody_t *)malloc(sizeof(cbody_t) * CACHE_MAX_ENTRIES);
    if (list == NULL)
        return cache->bytes;

    count = scan_cache(cache, list, CACHE_MAX_ENTRIES);
    for (i = 0; i < count; i++)
        total += list[i].size;
    free(list);

    cache->bytes = total;
    return total;
}

static void evict(tbox_cache_t *cache, const char *keep)
{
    cbody_t *list;
    int count;
    int i;

    list = (cbody_t *)malloc(sizeof(cbody_t) * CACHE_MAX_ENTRIES);
    if (list == NULL)
        return;

    count = scan_cache(cache, list, CACHE_MAX_ENTRIES);
    if (count > 1)
        qsort(list, (size_t)count, sizeof *list, cmp_oldest);

    for (i = 0; i < count && cache->bytes > cache->max_bytes; i++) {
        char path[PATHBUF];

        if (keep != NULL && strcmp(list[i].name, keep) == 0)
            continue;
        if (tbox_datadir_join(path, sizeof path, cache->root,
                              list[i].name) != 0)
            continue;
        if (remove(path) == 0)
            cache->bytes -= list[i].size;
    }

    if (cache->bytes < 0)
        cache->bytes = 0;

    free(list);
}

/* ---- open / close -------------------------------------------------------- */

int tbox_cache_open(tbox_cache_t *cache, const char *data_root,
                    long long max_bytes, tbox_cache_fetch_fn fetch,
                    void *fetch_user)
{
    cbody_t *list;
    int count;
    int i;

    memset(cache, 0, sizeof *cache);
    cache->max_bytes = max_bytes > 0 ? max_bytes : TBOX_CACHE_DEFAULT_MAX;
    cache->fetch = fetch;
    cache->fetch_user = fetch_user;

    if (tbox_datadir_cache(cache->root, sizeof cache->root, data_root) != 0) {
        fprintf(stderr, "read: data path is too long.\n");
        return -1;
    }
    if (tbox_datadir_ensure(cache->root) != 0) {
        fprintf(stderr, "read: cannot create %s\n", cache->root);
        return -1;
    }

    list = (cbody_t *)malloc(sizeof(cbody_t) * CACHE_MAX_ENTRIES);
    if (list == NULL) {
        fprintf(stderr, "read: out of memory.\n");
        return -1;
    }

    count = scan_cache(cache, list, CACHE_MAX_ENTRIES);
    for (i = 0; i < count; i++)
        cache->bytes += list[i].size;

    free(list);

    /* A cache over budget from an earlier run (a bigger limit, or bodies left
     * by a crash) is trimmed now rather than on the next fetch. */
    evict(cache, NULL);

    return 0;
}

void tbox_cache_close(tbox_cache_t *cache)
{
    memset(cache, 0, sizeof *cache);
}

long long tbox_cache_bytes(const tbox_cache_t *cache)
{
    return cache->bytes;
}

/* Fetch a body into a .part file and move it into place. */
static int ensure_body(tbox_cache_t *cache, long long file_id)
{
    char leaf[CFILE_MAX];
    char part[PATHBUF];
    char final[PATHBUF];
    long long size = 0;
    long long mtime = 0;
    int rc;

    if (body_leaf(leaf, sizeof leaf, file_id) != 0
        || body_path(final, sizeof final, cache, file_id) != 0
        || part_path(part, sizeof part, cache, file_id) != 0) {
        fprintf(stderr, "read: cache path is too long.\n");
        return -1;
    }

    rc = cache->fetch(cache->fetch_user, file_id, part);
    if (rc == 1) {
        remove(part);                        /* cancelled by the caller */
        return -2;
    }
    if (rc != 0) {
        remove(part);
        fprintf(stderr, "read: fetch of file %lld failed.\n", file_id);
        return -1;
    }

    /* C rename() refuses an existing target on Windows, and refuses it while
     * another thread has the body open. Either way the body we want is
     * already there, so losing the race is a success, not a failure. */
    if (rename(part, final) != 0) {
        remove(part);
        if (!file_exists(final))
            return -1;
        /* another thread installed it: our byte count never saw it */
        (void)remeasure(cache);
    } else if (stat_body(final, &size, &mtime) == 0) {
        cache->bytes += size;
    }

    evict(cache, leaf);

    return 0;
}

int tbox_cfile_open(tbox_cache_t *cache, long long file_id, tbox_cfile_t *out)
{
    char path[PATHBUF];
    FILE *fp;

    memset(out, 0, sizeof *out);
    out->file_id = file_id;
    out->cache = cache;

    if (file_id == 0) {
        fprintf(stderr, "read: this entry carries no file id, "
                        "so its bytes cannot be fetched.\n");
        return -1;
    }
    if (body_path(path, sizeof path, cache, file_id) != 0)
        return -1;

    if (!file_exists(path)) {
        int rc;

        if (cache->fetch == NULL) {
            fprintf(stderr, "read: this build serves names only, "
                            "so file %lld has no local body.\n", file_id);
            return -1;
        }
        rc = ensure_body(cache, file_id);
        if (rc != 0)
            return rc;               /* -1 failed, -2 cancelled */
    }

    fp = fopen(path, "rb");
    if (fp == NULL)
        return -1;

    out->fp = fp;
    out->size = 0;
    (void)stat_body(path, &out->size, NULL);

    return 0;
}

long long tbox_cfile_size(const tbox_cfile_t *f)
{
    return f->size;
}

long long tbox_cfile_read(tbox_cfile_t *f, void *buf, size_t len)
{
    size_t got;

    if (f == NULL || f->fp == NULL)
        return -1;
    got = fread(buf, 1, len, f->fp);
    if (got == 0 && ferror(f->fp))
        return -1;
    return (long long)got;
}

int tbox_cfile_seek(tbox_cfile_t *f, long long offset)
{
    if (f == NULL || f->fp == NULL)
        return -1;
    if (offset < 0 || offset > f->size)
        return -1;
    if (fseek(f->fp, (long)offset, SEEK_SET) != 0)
        return -1;
    return 0;
}

void tbox_cfile_close(tbox_cfile_t *f)
{
    if (f == NULL || f->fp == NULL)
        return;
    fclose(f->fp);
    f->fp = NULL;
    f->size = 0;
}