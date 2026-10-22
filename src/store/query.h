/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * query.h — read-only views of the resolved catalog for the protocol
 * commands catalog.list, catalog.stat and catalog.summary (spec 6.4, 7.6).
 *
 * This is the only place besides sync/ that reads `objects`/`messages`. The
 * rows are plain structs (no SQLite, no cJSON), so the engine decides how to
 * shape them on the wire and a test can check the data without a protocol.
 *
 * All ordering is explicit: folders are listed by name (case-insensitive)
 * then object id, so the UI gets a stable list; the Inbox and revisions are
 * newest first.
 */
#ifndef TBOX_STORE_QUERY_H
#define TBOX_STORE_QUERY_H

#include <stddef.h>

#include "catalog/message.h"
#include "catalog/resolve.h"
#include "store/db.h"

/* A name with room for the resolver's conflict suffix. */
#define TBOX_QUERY_NAME_MAX (TBOX_NAME_MAX_BYTES + 48)

/* One spec 7.6 item. Strings are fixed copies, so the array owns no heap. */
typedef struct TBoxListItem {
    char      object_id[TBOX_RESOLVE_ID_MAX + 1];
    char      kind; /* 'f' or 'd' */
    char      parent_id[TBOX_RESOLVE_ID_MAX + 1];
    char      name[TBOX_QUERY_NAME_MAX];
    long long rev;
    int       has_size;
    long long size;
    int       has_mtime;
    long long mtime;
    int       has_trashed;
    long long trashed_at;
    int       conflict;
    char      message_id[TBOX_MSGID_MAX_BYTES + 1];
    char      mime[96];
} TBoxListItem;

/* One Inbox item (spec 7.6): an unmanaged file that no record adopted. */
typedef struct TBoxInboxRow {
    char      message_id[TBOX_MSGID_MAX_BYTES + 1];
    char      orig_name[TBOX_NAME_MAX_BYTES * 4 + 1];
    char      mime[96];
    int       has_size;
    long long size;
    long long date;
} TBoxInboxRow;

/* One earlier (or the current) message of an object, for catalog.stat. */
typedef struct TBoxRevision {
    char      message_id[TBOX_MSGID_MAX_BYTES + 1];
    long long rev;
    long long date;
    int       has_size;
    long long size;
    int       has_sha256;
    char      sha256[TBOX_SHA256_HEX_LEN + 1];
    int       has_mtime;
    long long mtime;
    int       has_trashed;
    long long trashed_at;
} TBoxRevision;

/* Recursive totals for one top-level folder. */
typedef struct TBoxFolderTotal {
    char      folder_id[TBOX_RESOLVE_ID_MAX + 1];
    char      name[TBOX_QUERY_NAME_MAX];
    long long files;
    long long folders;
    long long bytes;
} TBoxFolderTotal;

/* catalog.summary. Every count is of live view rows; "bytes" is the sum of
 * known file sizes (folders contribute none). */
typedef struct TBoxCatalogSummary {
    long long files;   /* kind='f' anywhere */
    long long folders; /* kind='d' anywhere */
    long long bytes;
    long long root_files;
    long long root_folders;
    long long root_bytes;
    long long lost_files;
    long long lost_folders;
    long long lost_bytes;
    long long trash_files;
    long long trash_folders;
    long long trash_bytes;
    long long inbox_items;
    long long inbox_bytes;
    TBoxFolderTotal *top; /* folders whose parent is root, with subtree totals */
    size_t           top_count;
} TBoxCatalogSummary;

/* Lists one folder. folder_id is "root", "trash", "lost", or an object id.
 * Returns TBOX_DB_OK with *count possibly 0; TBOX_DB_E_ARGS for a bad id.
 * Free the array with tbox_query_free_items(). */
TBoxDbStatus tbox_query_list(TBoxDb *db, const char *folder_id,
                             TBoxListItem **items, size_t *count);

/* Reads one object row (including a conflict item). Returns TBOX_DB_OK and
 * sets *found; *item is untouched when not found. */
TBoxDbStatus tbox_query_get(TBoxDb *db, const char *object_id,
                            TBoxListItem *item, int *found);

/* Lists the Inbox in newest-first order. Free with tbox_query_free_inbox(). */
TBoxDbStatus tbox_query_inbox(TBoxDb *db, TBoxInboxRow **items,
                              size_t *count);

/* Lists every message carrying a record for `object_id` (a conflict id is
 * reduced to its base object), newest revision first. Free with
 * tbox_query_free_revisions(). */
TBoxDbStatus tbox_query_revisions(TBoxDb *db, const char *object_id,
                                  TBoxRevision **revs, size_t *count);

/* Builds catalog.summary. Free the top-folder array with
 * tbox_query_free_summary(). */
TBoxDbStatus tbox_query_summary(TBoxDb *db, TBoxCatalogSummary *out);

void tbox_query_free_items(TBoxListItem *items, size_t count);
void tbox_query_free_inbox(TBoxInboxRow *items, size_t count);
void tbox_query_free_revisions(TBoxRevision *revs, size_t count);
void tbox_query_free_summary(TBoxCatalogSummary *summary);

#endif /* TBOX_STORE_QUERY_H */
