/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TBOX_CORE_READ_H
#define TBOX_CORE_READ_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The read path (core layer): the local, bounded cache of archive file
 * bodies, and the offset/chunk reader a transport streams from.
 *
 * TDLib-free by rule. How bytes arrive is a single injected callback, so the
 * cache, its eviction policy and the reader are fully covered by
 * tests/test_read.c with a fake fetcher - no Telegram, no network.
 *
 * Why a cache at all: TDLib has no ranged reads, so the first open of a file
 * has to fetch all of it. Storing it means the second open is free and that
 * FTP transfers are instant. Because there is no server-side range support,
 * this layer presents read/seek itself and no transport ever needs to know.
 *
 * Layout:  <data>/cache/<remote file id>.bin
 * Safety:  a fetch lands in <data>/cache/.part-<id>-<n> and is then renamed
 *          over the final name, so a reader never sees a partial body.
 * Bounded: LRU by modification time; the oldest bodies go first once the
 *          directory exceeds its budget.
 */

/* 512 MiB: enough for a comfortable working set, small enough to be polite. */
#define TBOX_CACHE_DEFAULT_MAX (512LL * 1024LL * 1024LL)

/*
 * Fetch one file body into `dest_path`. The callback is the only thing that
 * knows about TDLib (it calls downloadFile) or about a test fixture.
 * Returns 0 when the file is ready, 1 when it was cancelled, -1 on error.
 */
typedef int (*tbox_cache_fetch_fn)(void *user, long long file_id,
                                   const char *dest_path);

typedef struct
{
    char root[512];              /* <data>/cache */
    long long max_bytes;         /* eviction budget */
    long long bytes;             /* current on-disk size of *.bin */
    tbox_cache_fetch_fn fetch;   /* injected: how bytes arrive */
    void *fetch_user;            /* passed back to fetch() */

} tbox_cache_t;

/*
 * Open the cache rooted at <data>/cache, creating the directory, and measure
 * what is already there so a restart does not re-download. max_bytes <= 0
 * means TBOX_CACHE_DEFAULT_MAX; it only takes effect on later fetches.
 *
 * fetch may be NULL for a read-only server (names list, bodies error out).
 * Returns 0, or -1 with a message printed.
 */
int tbox_cache_open(tbox_cache_t *cache, const char *data_root,
                    long long max_bytes, tbox_cache_fetch_fn fetch,
                    void *fetch_user);

void tbox_cache_close(tbox_cache_t *cache);

/* Current on-disk size of the cache, in bytes. */
long long tbox_cache_bytes(const tbox_cache_t *cache);

/* One open body. Not thread-safe: one handle per transport thread. */
typedef struct
{
    FILE *fp;
    long long size;
    long long file_id;
    tbox_cache_t *cache;         /* borrowed, for eviction bookkeeping */

} tbox_cfile_t;

/*
 * Get a readable handle on one file's bytes, downloading it on first use.
 *
 * Returns 0, -1 when the fetch failed (message printed), or -2 when the body
 * is not available at all (no fetcher injected, or fetch cancelled).
 */
int tbox_cfile_open(tbox_cache_t *cache, long long file_id, tbox_cfile_t *out);

/* Body size in bytes (0 for an empty file). */
long long tbox_cfile_size(const tbox_cfile_t *f);

/*
 * Read up to `len` bytes at the current position. Returns the number of
 * bytes read, or -1 on a read error. 0 means end of file.
 */
long long tbox_cfile_read(tbox_cfile_t *f, void *buf, size_t len);

/* Move the read position; returns 0, or -1 when out of range. */
int tbox_cfile_seek(tbox_cfile_t *f, long long offset);

void tbox_cfile_close(tbox_cfile_t *f);

#ifdef __cplusplus
}
#endif

#endif /* TBOX_CORE_READ_H */