/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TBOX_CORE_INDEXFILE_H
#define TBOX_CORE_INDEXFILE_H

#include "core/index.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * index.json - the archive mirror on disk.
 *
 * TDLib-free by rule, so it is covered by tests/test_indexfile.c with a
 * scratch path. It exists for two reasons and no others:
 *
 *   - a restart is instant: `tbox serve` shows the previous tree the moment
 *     it starts, instead of after the first scan has walked Saved Messages.
 *   - a refresh is diffable: two index.json files can be compared to see
 *     exactly what changed in Telegram.
 *
 * It is a *mirror*, never the source of truth: the scan rebuilds it, and a
 * missing or unreadable file is not an error - it just means "cold start".
 *
 * Format, one line per entry, compact and stable:
 *   {"v":1,"entries":[
 *     {"name":"a/b.txt","m":1234,"f":567,"z":11,"t":1700000000,"d":0},
 *     {"name":"photos","m":1235,"f":568,"z":0,"t":1700000001,"d":1}
 *   ]}
 *
 * Short keys, because a large Saved Messages makes this file long and it is
 * meant to be diffable by eye:
 *   name = archive name      m = message_id   f = remote file id
 *   z    = size              t = mtime        d = 1 for a directory
 *
 * `sha256`/`is_tbox` are deliberately NOT mirrored: they are re-derived from
 * the caption on the next scan, and a stale copy of a sha256 in a file that
 * merely looks like the archive would be worse than no copy at all.
 */

/* The mirror version, and the only one we accept. */
#define TBOX_INDEXFILE_V 1

/*
 * Write the archive to `path` atomically ("<path>.tmp" then rename over).
 * Returns 0, or -1 with a message printed.
 */
int tbox_indexfile_write(const char *path, const tbox_archive_t *ar);

/*
 * Load `path` into `ar`, appending every well-formed entry. Entries that are
 * not usable (no name, absurd numbers) are skipped rather than failing the
 * whole file, because a hand-edited mirror should degrade, not break.
 *
 * Returns the number of entries loaded, or -1 when the file could not be
 * read or is not a version 1 mirror (i.e. nothing was loaded either way).
 * A -1 here is not an error the caller must report: it just means "cold".
 */
long long tbox_indexfile_read(const char *path, tbox_archive_t *ar);

#ifdef __cplusplus
}
#endif

#endif /* TBOX_CORE_INDEXFILE_H */
