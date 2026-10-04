/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TBOX_DATADIR_H
#define TBOX_DATADIR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Where tbox keeps its state (util layer: no TDLib, no Telegram knowledge).
 *
 * One layout on every OS, so `serve` / `status` / `auth` always agree on
 * where the session lock and status.json live - even when the command runs
 * without TDLib in the binary:
 *
 *   <root>/tdlib         TDLib session database
 *   <root>/session.lock  exclusive owner lock (serve)
 *   <root>/status.json   service status (written by serve, read by status)
 *   <root>/cache         downloaded file bodies, bounded LRU (serve)
 *   <root>/index.json    archive index mirror, so a restart is instant
 *
 * <root> is --data <dir> when given, otherwise the per-OS default.
 */

/* Per-OS default root:
 *   Windows  %LOCALAPPDATA%\tbox              (USERPROFILE fallback)
 *   macOS    ~/Library/Application Support/tbox
 *   Linux    $XDG_DATA_HOME/tbox  else  ~/.local/share/tbox
 *
 * Returns 0, or -1 when no home directory can be found or the result
 * (NUL included) does not fit in `size`.
 */
int tbox_datadir_default(char *out, size_t size);

/*
 * Join "<root>/<leaf>" into `out`. A trailing separator on `root` is not
 * doubled. Returns 0, or -1 when the result would not fit.
 *
 * Length-checked memcpy, not snprintf: gcc's -Wformat-truncation (part of
 * -Wall, and this project builds with -Werror) cannot prove the "%s/%s"
 * form safe and fails the build.
 */
int tbox_datadir_join(char *out, size_t size, const char *root, const char *leaf);

/* "<root>/tdlib": the subdirectory TDLib owns exclusively. */
int tbox_datadir_session(char *out, size_t size, const char *root);

/* "<root>/cache": downloaded file bodies, one <file_id>.bin per file. */
int tbox_datadir_cache(char *out, size_t size, const char *root);

/* "<root>/index.json": the archive index mirror. */
int tbox_datadir_index(char *out, size_t size, const char *root);

/*
 * Create `path` and its missing parents. Best effort by design: an
 * existing directory counts as success, so callers do not have to check
 * first. Returns 0 when the directory exists afterwards, -1 otherwise.
 */
int tbox_datadir_ensure(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* TBOX_DATADIR_H */