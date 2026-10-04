/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 */

#ifndef TBOX_CORE_INDEX_H
#define TBOX_CORE_INDEX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Archive index (core layer): turns Saved Messages into the flat
 * "name -> message" ledger the whole app serves, and the only place that
 * knows the v0.3 caption schema.
 *
 * Pure by rule - no TDLib, no network, no filesystem - so it is fully
 * covered by tests/test_index.c. The TD layer feeds it parsed entries;
 * the transports (FTP now, SFTP later) and the CLI only read from it.
 *
 * The tree is derived, never stored:
 *   caption {"v":1,"type":"file","name":"a/b.txt","size":1,"mtime":2,"sha256":".."}
 *   caption {"v":1,"type":"dir","name":"a"}          explicit directory marker
 *   any name that is a prefix of another             implied directory
 *   no valid caption                                foreign document
 *
 * `message_id` and `file_id` are Telegram identifiers: the message an entry
 * came from and the remote file id used to download its bytes. Neither ever
 * appears in a caption, so the wire schema stays v0.3-exact; the td layer
 * fills them and `core/read` uses file_id as the cache key.
 */

#define TBOX_ARCHIVE_META_V 1     /* caption ledger version (v0.3 compatible) */
#define TBOX_ARCHIVE_MAX_NAME 512  /* longest archive name we accept */

/* One file or directory in the archive. */
typedef struct
{
    char name[TBOX_ARCHIVE_MAX_NAME]; /* logical path, no leading/trailing '/' */
    long long message_id;             /* Saved Messages message; -1 = implied dir */
    long long file_id;                /* Telegram remote file id; 0 when unknown */
    int is_dir;                       /* 1 = directory, 0 = file */
    long long size;                   /* bytes; 0 when unknown */
    long long mtime;                  /* unix seconds; -1 when unknown */
    char sha256[65];                  /* "" when unknown */
    int is_tbox;                      /* 1 = our caption, sha256 + mtime present */

} tbox_archive_entry_t;

/* Sentinel message id for directories that exist only as a name prefix. */
#define TBOX_MSG_NONE (-1)

/*
 * Normalize a client path into an archive name.
 *
 *   "/"      -> "" (the archive root)
 *   "/a//b/" -> "a/b"
 *   "a\\b"   -> "a/b"          (backslashes are folded)
 *   any whole ".." segment     -> rejected (returns -1, out untouched)
 *
 * "/..foo" is a legal name; only a full ".." segment is a traversal.
 * Returns 0 on success, -1 on an empty/invalid path or a too-long result.
 */
int tbox_archive_name(const char *path, char *out, size_t size);

/*
 * Parse a message caption into `out`.
 *
 * Tolerant by design (v0.3 rule): a missing caption, non-JSON, non-object
 * or wrong "v" all mean "foreign document" and return -1 without touching
 * `out`. On success, absent optional fields keep their defaults.
 */
int tbox_caption_parse(const char *caption, tbox_archive_entry_t *out);

/*
 * Serialize an entry back into its caption: compact separators, fixed key
 * order, no trailing newline (byte-compatible with v0.3, which the old
 * Python client still parses).
 *
 * Directories emit only {"v","type","name"}; files add size/mtime/sha256.
 * Caller owns the returned string (cJSON_free).
 */
char *tbox_caption_build(const tbox_archive_entry_t *entry);

/* ---- the index ------------------------------------------------------------- */

typedef struct
{
    tbox_archive_entry_t *items;  /* owned */
    size_t count;
    size_t capacity;
    int materialized;             /* implied directories already synthesized */

} tbox_archive_t;

void tbox_archive_init(tbox_archive_t *ar);
void tbox_archive_free(tbox_archive_t *ar);

/*
 * Append an entry. `name` is normalized with tbox_archive_name(); an
 * invalid name is rejected (returns -1, index unchanged). Re-adding the
 * same name replaces the existing entry in place (newest wins), which is
 * what makes upload-then-delete replacement resolve deterministically.
 */
int tbox_archive_add(tbox_archive_t *ar, const tbox_archive_entry_t *entry);

/* Add implied directories for every name prefix not already present.
 * Idempotent; called automatically by the read paths. */
void tbox_archive_materialize(tbox_archive_t *ar);

/* Look up one entry by archive name; NULL when absent. */
const tbox_archive_entry_t *tbox_archive_find(tbox_archive_t *ar,
                                              const char *path);

/*
 * List the direct children of `path` ("" or "/" = root), sorted by name
 * (byte order, directories and files interleaved - the protocol rule, not
 * the CLI's dirs-first rule).
 *
 * On success *out holds *cap entries: the children, or two temporary
 * "." / ".." directory records when the directory is empty, so clients
 * never see an empty listing (WinSCP pops up otherwise).
 *
 * Entries are plain values - release *out with free().
 *
 * Returns 0 on success, -1 on an invalid path (no allocation) or -2 when
 * the path does not exist.
 */
int tbox_archive_list(tbox_archive_t *ar, const char *path,
                      tbox_archive_entry_t **out, size_t *cap);

/*
 * stat() semantics for one path: 0 = found (out filled), -1 = not found.
 * The root is always a directory; anything that is a name prefix of an
 * entry is a directory.
 */
int tbox_archive_stat(tbox_archive_t *ar, const char *path,
                      tbox_archive_entry_t *out);

/* Sort by name, byte order (strcmp). Used after a bulk load. */
void tbox_archive_sort(tbox_archive_t *ar);

#ifdef __cplusplus
}
#endif

#endif /* TBOX_CORE_INDEX_H */