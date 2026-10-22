/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * query.c — read-only catalog queries (see query.h).
 *
 * Every value is bound, never spliced, and every statement reads only the
 * derived tables. Nothing here touches TDLib or the filesystem.
 */
#include "store/query.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite3.h"

/* ---- small helpers --------------------------------------------------- */

/* Copies a column's text into a fixed buffer (empty for NULL). */
static void copy_col(sqlite3_stmt *st, int col, char *dst, size_t cap) {
    const unsigned char *txt = sqlite3_column_text(st, col);
    if (txt == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, cap, "%s", (const char *)txt);
}

/* Reads one spec 7.6 item from the fixed column order used by the object
 * queries:
 *   0 object_id, 1 kind, 2 parent_id, 3 name, 4 rev, 5 size, 6 mtime,
 *   7 trashed_at, 8 conflict, 9 message_id, 10 mime
 */
static void read_item(sqlite3_stmt *st, TBoxListItem *it) {
    copy_col(st, 0, it->object_id, sizeof it->object_id);
    copy_col(st, 1, &it->kind, 2);
    copy_col(st, 2, it->parent_id, sizeof it->parent_id);
    copy_col(st, 3, it->name, sizeof it->name);
    it->rev = sqlite3_column_int64(st, 4);
    if (sqlite3_column_type(st, 5) != SQLITE_NULL) {
        it->has_size = 1;
        it->size = sqlite3_column_int64(st, 5);
    }
    if (sqlite3_column_type(st, 6) != SQLITE_NULL) {
        it->has_mtime = 1;
        it->mtime = sqlite3_column_int64(st, 6);
    }
    if (sqlite3_column_type(st, 7) != SQLITE_NULL) {
        it->has_trashed = 1;
        it->trashed_at = sqlite3_column_int64(st, 7);
    }
    it->conflict = sqlite3_column_int(st, 8) != 0;
    copy_col(st, 9, it->message_id, sizeof it->message_id);
    copy_col(st, 10, it->mime, sizeof it->mime);
}

static const char *const OBJECT_SELECT =
    "SELECT o.object_id,o.kind,o.parent_id,o.name,o.rev,o.size,o.mtime,"
    "o.trashed_at,o.conflict,o.message_id,m.mime "
    "FROM objects o LEFT JOIN messages m ON m.message_id=o.message_id ";

/* ---- listing --------------------------------------------------------- */

TBoxDbStatus tbox_query_list(TBoxDb *db, const char *folder_id,
                             TBoxListItem **items, size_t *count) {
    if (db == NULL || folder_id == NULL || folder_id[0] == '\0' ||
        items == NULL || count == NULL) {
        return TBOX_DB_E_ARGS;
    }
    *items = NULL;
    *count = 0;
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL) {
        return TBOX_DB_E_ARGS;
    }

    char sql[512];
    snprintf(sql, sizeof sql,
             "%sWHERE o.parent_id=?1 "
             "ORDER BY o.name COLLATE NOCASE,o.object_id",
             OBJECT_SELECT);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) != SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    sqlite3_bind_text(st, 1, folder_id, -1, SQLITE_TRANSIENT);

    size_t cap = 0;
    size_t n = 0;
    TBoxListItem *out = NULL;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n == cap) {
            size_t ncap = cap == 0 ? 16 : cap * 2;
            TBoxListItem *grown = realloc(out, ncap * sizeof *grown);
            if (grown == NULL) {
                free(out);
                sqlite3_finalize(st);
                return TBOX_DB_E_MEM;
            }
            memset(grown + cap, 0, (ncap - cap) * sizeof *grown);
            out = grown;
            cap = ncap;
        }
        read_item(st, &out[n]);
        n++;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        free(out);
        return TBOX_DB_E_IO;
    }
    *items = out;
    *count = n;
    return TBOX_DB_OK;
}

TBoxDbStatus tbox_query_get(TBoxDb *db, const char *object_id,
                            TBoxListItem *item, int *found) {
    if (db == NULL || object_id == NULL || object_id[0] == '\0' ||
        item == NULL || found == NULL) {
        return TBOX_DB_E_ARGS;
    }
    *found = 0;
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL) {
        return TBOX_DB_E_ARGS;
    }
    char sql[512];
    snprintf(sql, sizeof sql, "%sWHERE o.object_id=?1", OBJECT_SELECT);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) != SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    sqlite3_bind_text(st, 1, object_id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        memset(item, 0, sizeof *item);
        read_item(st, item);
        *found = 1;
    }
    sqlite3_finalize(st);
    return rc == SQLITE_ROW || rc == SQLITE_DONE ? TBOX_DB_OK : TBOX_DB_E_IO;
}

/* ---- inbox ----------------------------------------------------------- */

/* The spec 7.4 Inbox rule as SQL: unmanaged files whose message id is not
 * some record's `src`. */
static const char *const INBOX_WHERE =
    "WHERE kind='unmanaged' AND message_id NOT IN "
    "(SELECT rec_src FROM messages WHERE rec_src IS NOT NULL)";

TBoxDbStatus tbox_query_inbox(TBoxDb *db, TBoxInboxRow **items,
                              size_t *count) {
    if (db == NULL || items == NULL || count == NULL) {
        return TBOX_DB_E_ARGS;
    }
    *items = NULL;
    *count = 0;
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL) {
        return TBOX_DB_E_ARGS;
    }
    char sql[512];
    snprintf(sql, sizeof sql,
             "SELECT message_id,orig_name,mime,size,date FROM messages %s "
             "ORDER BY date DESC,CAST(message_id AS INTEGER) DESC",
             INBOX_WHERE);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) != SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    size_t cap = 0;
    size_t n = 0;
    TBoxInboxRow *out = NULL;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n == cap) {
            size_t ncap = cap == 0 ? 16 : cap * 2;
            TBoxInboxRow *grown = realloc(out, ncap * sizeof *grown);
            if (grown == NULL) {
                free(out);
                sqlite3_finalize(st);
                return TBOX_DB_E_MEM;
            }
            memset(grown + cap, 0, (ncap - cap) * sizeof *grown);
            out = grown;
            cap = ncap;
        }
        TBoxInboxRow *r = &out[n++];
        copy_col(st, 0, r->message_id, sizeof r->message_id);
        copy_col(st, 1, r->orig_name, sizeof r->orig_name);
        copy_col(st, 2, r->mime, sizeof r->mime);
        if (sqlite3_column_type(st, 3) != SQLITE_NULL) {
            r->has_size = 1;
            r->size = sqlite3_column_int64(st, 3);
        }
        r->date = sqlite3_column_int64(st, 4);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        free(out);
        return TBOX_DB_E_IO;
    }
    *items = out;
    *count = n;
    return TBOX_DB_OK;
}

/* ---- revisions ------------------------------------------------------- */

/* A conflict id is "<o>-<message id>"; the messages table keys on <o>. */
static void base_object_id(const char *object_id, char *out, size_t cap) {
    const char *dash = strchr(object_id, '-');
    size_t n = dash != NULL ? (size_t)(dash - object_id) : strlen(object_id);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, object_id, n);
    out[n] = '\0';
}

TBoxDbStatus tbox_query_revisions(TBoxDb *db, const char *object_id,
                                  TBoxRevision **revs, size_t *count) {
    if (db == NULL || object_id == NULL || object_id[0] == '\0' ||
        revs == NULL || count == NULL) {
        return TBOX_DB_E_ARGS;
    }
    *revs = NULL;
    *count = 0;
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL) {
        return TBOX_DB_E_ARGS;
    }
    char base[TBOX_RESOLVE_ID_MAX + 1];
    base_object_id(object_id, base, sizeof base);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(
            h,
            "SELECT message_id,rec_r,date,size,rec_h,rec_t,text FROM messages "
            "WHERE rec_o=?1 ORDER BY rec_r DESC,CAST(message_id AS INTEGER) "
            "DESC",
            -1, &st, NULL) != SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    sqlite3_bind_text(st, 1, base, -1, SQLITE_TRANSIENT);

    size_t cap = 0;
    size_t n = 0;
    TBoxRevision *out = NULL;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n == cap) {
            size_t ncap = cap == 0 ? 8 : cap * 2;
            TBoxRevision *grown = realloc(out, ncap * sizeof *grown);
            if (grown == NULL) {
                free(out);
                sqlite3_finalize(st);
                return TBOX_DB_E_MEM;
            }
            memset(grown + cap, 0, (ncap - cap) * sizeof *grown);
            out = grown;
            cap = ncap;
        }
        TBoxRevision *r = &out[n++];
        copy_col(st, 0, r->message_id, sizeof r->message_id);
        r->rev = sqlite3_column_int64(st, 1);
        r->date = sqlite3_column_int64(st, 2);
        if (sqlite3_column_type(st, 3) != SQLITE_NULL) {
            r->has_size = 1;
            r->size = sqlite3_column_int64(st, 3);
        }
        if (sqlite3_column_type(st, 4) != SQLITE_NULL) {
            r->has_sha256 = 1;
            copy_col(st, 4, r->sha256, sizeof r->sha256);
        }
        if (sqlite3_column_type(st, 5) != SQLITE_NULL) {
            r->has_trashed = 1;
            r->trashed_at = sqlite3_column_int64(st, 5);
        }
        /* m is not a column (spec 7.5); the caption is, so re-parse it. */
        const unsigned char *text = sqlite3_column_text(st, 6);
        if (text != NULL) {
            TBoxRecord rec;
            if (tbox_record_parse((const char *)text, &rec) == TBOX_REC_OK &&
                rec.has_mtime) {
                r->has_mtime = 1;
                r->mtime = rec.mtime;
            }
        }
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        free(out);
        return TBOX_DB_E_IO;
    }
    *revs = out;
    *count = n;
    return TBOX_DB_OK;
}

/* ---- summary --------------------------------------------------------- */

/* Adds one row's numbers to a (files, folders, bytes) triple. */
static void add_counts(long long *files, long long *folders, long long *bytes,
                       const char *kind, int has_size, long long size) {
    if (strcmp(kind, "f") == 0) {
        (*files)++;
        if (has_size) {
            *bytes += size;
        }
    } else {
        (*folders)++;
    }
}

TBoxDbStatus tbox_query_summary(TBoxDb *db, TBoxCatalogSummary *out) {
    if (db == NULL || out == NULL) {
        return TBOX_DB_E_ARGS;
    }
    memset(out, 0, sizeof *out);
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL) {
        return TBOX_DB_E_ARGS;
    }

    /* totals per virtual folder, per kind */
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(
            h,
            "SELECT parent_id,kind,count(*),coalesce(sum(size),0) FROM objects "
            "WHERE parent_id IN ('root','lost','trash') "
            "GROUP BY parent_id,kind",
            -1, &st, NULL) != SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        char parent[TBOX_RESOLVE_ID_MAX + 1];
        char kind[8];
        copy_col(st, 0, parent, sizeof parent);
        copy_col(st, 1, kind, sizeof kind);
        long long b = sqlite3_column_int64(st, 3);
        if (strcmp(parent, TBOX_FOLDER_ROOT) == 0) {
            add_counts(&out->root_files, &out->root_folders, &out->root_bytes,
                       kind, 1, b);
        } else if (strcmp(parent, TBOX_FOLDER_LOST) == 0) {
            add_counts(&out->lost_files, &out->lost_folders, &out->lost_bytes,
                       kind, 1, b);
        } else {
            add_counts(&out->trash_files, &out->trash_folders,
                       &out->trash_bytes, kind, 1, b);
        }
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        return TBOX_DB_E_IO;
    }

    /* totals per type over the whole catalog */
    if (sqlite3_prepare_v2(h,
                           "SELECT kind,count(*),coalesce(sum(size),0) FROM "
                           "objects GROUP BY kind",
                           -1, &st, NULL) != SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        char kind[8];
        copy_col(st, 0, kind, sizeof kind);
        long long c = sqlite3_column_int64(st, 1);
        long long b = sqlite3_column_int64(st, 2);
        if (strcmp(kind, "f") == 0) {
            out->files = c;
            out->bytes = b;
        } else {
            out->folders = c;
        }
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        return TBOX_DB_E_IO;
    }

    /* the Inbox is computed, not stored */
    char sql[512];
    snprintf(sql, sizeof sql,
             "SELECT count(*),coalesce(sum(size),0) FROM messages %s",
             INBOX_WHERE);
    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) != SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    if (sqlite3_step(st) == SQLITE_ROW) {
        out->inbox_items = sqlite3_column_int64(st, 0);
        out->inbox_bytes = sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);

    /* top-level folders, each with its whole subtree counted */
    if (sqlite3_prepare_v2(
            h,
            "SELECT object_id,name FROM objects WHERE parent_id='root' AND "
            "kind='d' ORDER BY name COLLATE NOCASE,object_id",
            -1, &st, NULL) != SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    size_t cap = 0;
    size_t n = 0;
    TBoxFolderTotal *tops = NULL;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (n == cap) {
            size_t ncap = cap == 0 ? 8 : cap * 2;
            TBoxFolderTotal *grown = realloc(tops, ncap * sizeof *grown);
            if (grown == NULL) {
                free(tops);
                sqlite3_finalize(st);
                return TBOX_DB_E_MEM;
            }
            memset(grown + cap, 0, (ncap - cap) * sizeof *grown);
            tops = grown;
            cap = ncap;
        }
        TBoxFolderTotal *t = &tops[n++];
        copy_col(st, 0, t->folder_id, sizeof t->folder_id);
        copy_col(st, 1, t->name, sizeof t->name);
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        free(tops);
        return TBOX_DB_E_IO;
    }

    static const char *const SUBTREE_SQL =
        "WITH RECURSIVE sub(object_id) AS ("
        "  SELECT object_id FROM objects WHERE parent_id=?1"
        "  UNION"
        "  SELECT o.object_id FROM objects o JOIN sub ON o.parent_id=sub.object_id"
        ") "
        "SELECT coalesce(sum(CASE WHEN o.kind='f' THEN 1 ELSE 0 END),0),"
        "       coalesce(sum(CASE WHEN o.kind='d' THEN 1 ELSE 0 END),0),"
        "       coalesce(sum(o.size),0) "
        "FROM sub JOIN objects o ON o.object_id=sub.object_id";
    for (size_t i = 0; i < n; i++) {
        if (sqlite3_prepare_v2(h, SUBTREE_SQL, -1, &st, NULL) != SQLITE_OK) {
            free(tops);
            return TBOX_DB_E_IO;
        }
        sqlite3_bind_text(st, 1, tops[i].folder_id, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            tops[i].files = sqlite3_column_int64(st, 0);
            tops[i].folders = sqlite3_column_int64(st, 1);
            tops[i].bytes = sqlite3_column_int64(st, 2);
        }
        sqlite3_finalize(st);
    }
    out->top = tops;
    out->top_count = n;
    return TBOX_DB_OK;
}

/* ---- freeing --------------------------------------------------------- */

void tbox_query_free_items(TBoxListItem *items, size_t count) {
    (void)count;
    free(items);
}

void tbox_query_free_inbox(TBoxInboxRow *items, size_t count) {
    (void)count;
    free(items);
}

void tbox_query_free_revisions(TBoxRevision *revs, size_t count) {
    (void)count;
    free(revs);
}

void tbox_query_free_summary(TBoxCatalogSummary *summary) {
    if (summary == NULL) {
        return;
    }
    free(summary->top);
    summary->top = NULL;
    summary->top_count = 0;
}
