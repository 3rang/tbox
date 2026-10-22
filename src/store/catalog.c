/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * catalog.c — the catalog store (see catalog.h). Every statement is prepared
 * once per call, every value is bound, and the connection is only ever the
 * engine's own (spec 3.1 rule 2).
 */
#include "store/catalog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite3.h"

/* Runs a no-parameter statement, mapping the result to a status. */
static TBoxDbStatus exec(sqlite3 *h, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(h, sql, NULL, NULL, &err);
    sqlite3_free(err);
    return rc == SQLITE_OK ? TBOX_DB_OK : TBOX_DB_E_IO;
}

TBoxDbStatus tbox_cat_begin(TBoxDb *db) {
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL) {
        return TBOX_DB_E_ARGS;
    }
    return exec(h, "BEGIN");
}

TBoxDbStatus tbox_cat_commit(TBoxDb *db) {
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL) {
        return TBOX_DB_E_ARGS;
    }
    return exec(h, "COMMIT");
}

TBoxDbStatus tbox_cat_rollback(TBoxDb *db) {
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL) {
        return TBOX_DB_E_ARGS;
    }
    return exec(h, "ROLLBACK");
}

/* Binds text that is always present (empty string when NULL). */
static void bind_text(sqlite3_stmt *st, int idx, const char *s) {
    sqlite3_bind_text(st, idx, s != NULL ? s : "", -1, SQLITE_TRANSIENT);
}

TBoxDbStatus tbox_cat_upsert_message(TBoxDb *db, const TBoxMessageRow *row) {
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL || row == NULL) {
        return TBOX_DB_E_ARGS;
    }
    static const char *const SQL =
        "INSERT INTO messages(message_id,kind,date,file_id,remote_id,mime,"
        "size,orig_name,text,rec_o,rec_p,rec_n,rec_r,rec_k,rec_t,rec_h,rec_q,"
        "rec_src,bad_record) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,"
        "?13,?14,?15,?16,?17,?18,?19) ON CONFLICT(message_id) DO UPDATE SET "
        "kind=excluded.kind,date=excluded.date,file_id=excluded.file_id,"
        "remote_id=excluded.remote_id,mime=excluded.mime,size=excluded.size,"
        "orig_name=excluded.orig_name,text=excluded.text,rec_o=excluded.rec_o,"
        "rec_p=excluded.rec_p,rec_n=excluded.rec_n,rec_r=excluded.rec_r,"
        "rec_k=excluded.rec_k,rec_t=excluded.rec_t,rec_h=excluded.rec_h,"
        "rec_q=excluded.rec_q,rec_src=excluded.rec_src,"
        "bad_record=excluded.bad_record";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h, SQL, -1, &st, NULL) != SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    bind_text(st, 1, row->message_id);
    bind_text(st, 2, row->kind);
    sqlite3_bind_int64(st, 3, row->date);
    if (row->has_file_id) {
        sqlite3_bind_int64(st, 4, row->file_id);
    } else {
        sqlite3_bind_null(st, 4);
    }
    if (row->has_remote_id) {
        sqlite3_bind_int64(st, 5, row->remote_id);
    } else {
        sqlite3_bind_null(st, 5);
    }
    bind_text(st, 6, row->mime);
    if (row->has_size) {
        sqlite3_bind_int64(st, 7, row->size);
    } else {
        sqlite3_bind_null(st, 7);
    }
    bind_text(st, 8, row->orig_name);
    bind_text(st, 9, row->text);

    /* record fields: all NULL when there is no valid record, so resolve can
     * tell a managed row from an unmanaged one at a glance */
    if (row->has_record) {
        bind_text(st, 10, row->rec.object_id);
        bind_text(st, 11, row->rec.parent);
        bind_text(st, 12, row->rec.name);
        sqlite3_bind_int64(st, 13, row->rec.rev);
        char kind[2] = { row->rec.kind, '\0' };
        bind_text(st, 14, kind);
        if (row->rec.has_trashed) {
            sqlite3_bind_int64(st, 15, row->rec.trashed_at);
        } else {
            sqlite3_bind_null(st, 15);
        }
        if (row->rec.has_sha256) {
            bind_text(st, 16, row->rec.sha256);
        } else {
            sqlite3_bind_null(st, 16);
        }
        if (row->rec.has_op) {
            bind_text(st, 17, row->rec.op_id);
        } else {
            sqlite3_bind_null(st, 17);
        }
        if (row->rec.has_src) {
            bind_text(st, 18, row->rec.src);
        } else {
            sqlite3_bind_null(st, 18);
        }
    } else {
        for (int i = 10; i <= 18; i++) {
            sqlite3_bind_null(st, i);
        }
    }
    sqlite3_bind_int(st, 19, row->bad_record ? 1 : 0);

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? TBOX_DB_OK : TBOX_DB_E_IO;
}

int tbox_cat_has_message(TBoxDb *db, const char *message_id) {
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL || message_id == NULL) {
        return 0;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h, "SELECT 1 FROM messages WHERE message_id=?1", -1,
                           &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(st, 1, message_id, -1, SQLITE_TRANSIENT);
    int found = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return found;
}

TBoxDbStatus tbox_cat_delete_messages(TBoxDb *db, const char *const *ids,
                                      size_t count) {
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL) {
        return TBOX_DB_E_ARGS;
    }
    if (count == 0) {
        return TBOX_DB_OK;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h, "DELETE FROM messages WHERE message_id=?1", -1,
                           &st, NULL) != SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    TBoxDbStatus status = TBOX_DB_OK;
    for (size_t i = 0; i < count; i++) {
        sqlite3_bind_text(st, 1, ids[i], -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_DONE) {
            status = TBOX_DB_E_IO;
        }
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    return status;
}

TBoxDbStatus tbox_cat_remap_temp_message(TBoxDb *db, const char *old_id,
                                         const char *new_id) {
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL || old_id == NULL || new_id == NULL) {
        return TBOX_DB_E_ARGS;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(
            h, "UPDATE ops SET temp_msg_id=?2 WHERE temp_msg_id=?1", -1, &st,
            NULL) != SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    sqlite3_bind_text(st, 1, old_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, new_id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? TBOX_DB_OK : TBOX_DB_E_IO;
}

TBoxDbStatus tbox_cat_rebuild_objects(TBoxDb *db, long long *objects,
                                      long long *inbox) {
    TBoxMessageRow *rows = NULL;
    size_t count = 0;
    if (tbox_cat_load_messages(db, &rows, &count) != TBOX_DB_OK) {
        return TBOX_DB_E_IO;
    }
    TBoxResolveMsg *views = NULL;
    if (count > 0) {
        views = calloc(count, sizeof *views);
        if (views == NULL) {
            tbox_cat_free_messages(rows, count);
            return TBOX_DB_E_MEM;
        }
        for (size_t i = 0; i < count; i++) {
            const TBoxMessageRow *r = &rows[i];
            TBoxResolveMsg *v = &views[i];
            memset(v, 0, sizeof *v);
            v->message_id = r->message_id;
            v->date = r->date;
            v->orig_name = r->orig_name[0] != '\0' ? r->orig_name : NULL;
            v->mime = r->mime[0] != '\0' ? r->mime : NULL;
            v->has_size = r->has_size;
            v->size = r->size;
            v->bad_record = r->bad_record;
            if (strcmp(r->kind, TBOX_MSG_KIND_FILE) == 0) {
                v->kind = TBOX_MSG_FILE;
                v->rec = r->rec;
            } else if (strcmp(r->kind, TBOX_MSG_KIND_FOLDER) == 0) {
                v->kind = TBOX_MSG_FOLDER;
                v->rec = r->rec;
            } else if (strcmp(r->kind, TBOX_MSG_KIND_UNMANAGED) == 0) {
                v->kind = TBOX_MSG_UNMANAGED;
            } else {
                v->kind = TBOX_MSG_NOTE;
            }
        }
    }
    TBoxResolveResult res;
    memset(&res, 0, sizeof res);
    int ok = 0;
    if (tbox_resolve(views, count, &res) == 0) {
        if (objects != NULL) {
            *objects = (long long)res.object_count;
        }
        if (inbox != NULL) {
            *inbox = (long long)res.inbox_count;
        }
        ok = tbox_cat_replace_objects(db, &res) == TBOX_DB_OK;
        tbox_resolve_result_free(&res);
    }
    free(views);
    tbox_cat_free_messages(rows, count);
    return ok ? TBOX_DB_OK : TBOX_DB_E_IO;
}

TBoxDbStatus tbox_cat_replace_objects(TBoxDb *db,
                                      const TBoxResolveResult *res) {
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL || res == NULL) {
        return TBOX_DB_E_ARGS;
    }
    if (tbox_cat_begin(db) != TBOX_DB_OK) {
        return TBOX_DB_E_IO;
    }
    if (exec(h, "DELETE FROM objects") != TBOX_DB_OK) {
        (void)tbox_cat_rollback(db);
        return TBOX_DB_E_IO;
    }
    static const char *const SQL =
        "INSERT INTO objects(object_id,kind,parent_id,name,rev,message_id,"
        "size,sha256,mtime,trashed_at,conflict) VALUES(?1,?2,?3,?4,?5,?6,?7,"
        "?8,?9,?10,?11)";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h, SQL, -1, &st, NULL) != SQLITE_OK) {
        (void)tbox_cat_rollback(db);
        return TBOX_DB_E_IO;
    }
    for (size_t i = 0; i < res->object_count; i++) {
        const TBoxObject *o = &res->objects[i];
        char kind[2] = { o->kind, '\0' };
        bind_text(st, 1, o->object_id);
        bind_text(st, 2, kind);
        bind_text(st, 3, o->parent_id);
        bind_text(st, 4, o->name);
        sqlite3_bind_int64(st, 5, o->rev);
        bind_text(st, 6, o->message_id);
        if (o->has_size) {
            sqlite3_bind_int64(st, 7, o->size);
        } else {
            sqlite3_bind_null(st, 7);
        }
        if (o->sha256 != NULL) {
            bind_text(st, 8, o->sha256);
        } else {
            sqlite3_bind_null(st, 8);
        }
        if (o->has_mtime) {
            sqlite3_bind_int64(st, 9, o->mtime);
        } else {
            sqlite3_bind_null(st, 9);
        }
        if (o->has_trashed) {
            sqlite3_bind_int64(st, 10, o->trashed_at);
        } else {
            sqlite3_bind_null(st, 10);
        }
        sqlite3_bind_int(st, 11, o->conflict ? 1 : 0);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            (void)tbox_cat_rollback(db);
            return TBOX_DB_E_IO;
        }
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    if (tbox_cat_commit(db) != TBOX_DB_OK) {
        return TBOX_DB_E_IO;
    }
    return TBOX_DB_OK;
}

TBoxDbStatus tbox_cat_list_message_ids(TBoxDb *db, char ***ids, size_t *count) {
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL || ids == NULL || count == NULL) {
        return TBOX_DB_E_ARGS;
    }
    *ids = NULL;
    *count = 0;
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h, "SELECT message_id FROM messages", -1, &st,
                           NULL) != SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    char **out = NULL;
    size_t n = 0;
    size_t cap = 0;
    TBoxDbStatus status = TBOX_DB_OK;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        const unsigned char *txt = sqlite3_column_text(st, 0);
        const char *s = txt != NULL ? (const char *)txt : "";
        if (n == cap) {
            size_t ncap = cap == 0 ? 16 : cap * 2;
            char **grown = realloc(out, ncap * sizeof *grown);
            if (grown == NULL) {
                status = TBOX_DB_E_IO;
                break;
            }
            out = grown;
            cap = ncap;
        }
        size_t len = strlen(s);
        char *copy = malloc(len + 1);
        if (copy == NULL) {
            status = TBOX_DB_E_IO;
            break;
        }
        memcpy(copy, s, len + 1);
        out[n++] = copy;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE && status == TBOX_DB_OK) {
        status = TBOX_DB_E_IO;
    }
    if (status != TBOX_DB_OK) {
        tbox_cat_free_ids(out, n);
        return status;
    }
    *ids = out;
    *count = n;
    return TBOX_DB_OK;
}

void tbox_cat_free_ids(char **ids, size_t count) {
    if (ids == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(ids[i]);
    }
    free(ids);
}

/* Copies a column's text into a fixed buffer (empty string for NULL or when it
 * does not fit). */
static void copy_col(sqlite3_stmt *st, int col, char *dst, size_t cap) {
    const unsigned char *txt = sqlite3_column_text(st, col);
    if (txt == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, cap, "%s", (const char *)txt);
}

/* Copies a column's text into a fresh heap string, or NULL when the column is
 * NULL or empty. */
static char *dup_col(sqlite3_stmt *st, int col) {
    const unsigned char *txt = sqlite3_column_text(st, col);
    if (txt == NULL) {
        return NULL;
    }
    size_t n = strlen((const char *)txt);
    char *p = malloc(n + 1);
    if (p != NULL) {
        memcpy(p, txt, n + 1);
    }
    return p;
}

TBoxDbStatus tbox_cat_load_messages(TBoxDb *db, TBoxMessageRow **rows,
                                    size_t *count) {
    sqlite3 *h = tbox_db_handle(db);
    if (h == NULL || rows == NULL || count == NULL) {
        return TBOX_DB_E_ARGS;
    }
    *rows = NULL;
    *count = 0;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(h, "SELECT count(*) FROM messages", -1, &st, NULL) !=
        SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    size_t n = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        n = (size_t)sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    if (n == 0) {
        return TBOX_DB_OK;
    }

    TBoxMessageRow *out = calloc(n, sizeof *out);
    if (out == NULL) {
        return TBOX_DB_E_MEM;
    }

    static const char *const SQL =
        "SELECT message_id,kind,date,file_id,remote_id,mime,size,orig_name,"
        "text,rec_o,rec_p,rec_n,rec_r,rec_k,rec_t,rec_h,rec_q,rec_src,"
        "bad_record FROM messages";
    if (sqlite3_prepare_v2(h, SQL, -1, &st, NULL) != SQLITE_OK) {
        free(out);
        return TBOX_DB_E_IO;
    }

    size_t i = 0;
    int rc;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW && i < n) {
        TBoxMessageRow *r = &out[i++];
        copy_col(st, 0, r->message_id, sizeof r->message_id);
        copy_col(st, 1, r->kind, sizeof r->kind);
        r->date = sqlite3_column_int64(st, 2);
        if (sqlite3_column_type(st, 3) != SQLITE_NULL) {
            r->has_file_id = 1;
            r->file_id = sqlite3_column_int64(st, 3);
        }
        if (sqlite3_column_type(st, 4) != SQLITE_NULL) {
            r->has_remote_id = 1;
            r->remote_id = sqlite3_column_int64(st, 4);
        }
        copy_col(st, 5, r->mime, sizeof r->mime);
        if (sqlite3_column_type(st, 6) != SQLITE_NULL) {
            r->has_size = 1;
            r->size = sqlite3_column_int64(st, 6);
        }
        copy_col(st, 7, r->orig_name, sizeof r->orig_name);
        r->text = dup_col(st, 8);
        if (sqlite3_column_type(st, 9) != SQLITE_NULL) {
            r->has_record = 1;
            copy_col(st, 9, r->rec.object_id, sizeof r->rec.object_id);
            copy_col(st, 10, r->rec.parent, sizeof r->rec.parent);
            copy_col(st, 11, r->rec.name, sizeof r->rec.name);
            r->rec.rev = sqlite3_column_int64(st, 12);
            char k[4];
            copy_col(st, 13, k, sizeof k);
            r->rec.kind = k[0];
            if (sqlite3_column_type(st, 14) != SQLITE_NULL) {
                r->rec.has_trashed = 1;
                r->rec.trashed_at = sqlite3_column_int64(st, 14);
            }
            if (sqlite3_column_type(st, 15) != SQLITE_NULL) {
                r->rec.has_sha256 = 1;
                copy_col(st, 15, r->rec.sha256, sizeof r->rec.sha256);
            }
            if (sqlite3_column_type(st, 16) != SQLITE_NULL) {
                r->rec.has_op = 1;
                copy_col(st, 16, r->rec.op_id, sizeof r->rec.op_id);
            }
            if (sqlite3_column_type(st, 17) != SQLITE_NULL) {
                r->rec.has_src = 1;
                copy_col(st, 17, r->rec.src, sizeof r->rec.src);
            }
        }
        /* The spec 7.5 messages table stores the record's indexed fields but
         * not every key (there is no rec_s/rec_m column). The caption itself
         * is stored in `text`, so re-parsing it recovers the record exactly.
         * That keeps a row loaded here identical to one just parsed by the
         * scan, which is what the "incremental == full scan" invariant needs. */
        if (r->has_record && r->text != NULL) {
            TBoxRecord parsed;
            if (tbox_record_parse(r->text, &parsed) == TBOX_REC_OK) {
                r->rec = parsed;
            }
        }
        r->bad_record = sqlite3_column_int(st, 18) != 0;
    }
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        tbox_cat_free_messages(out, i);
        return TBOX_DB_E_IO;
    }
    *rows = out;
    *count = i;
    return TBOX_DB_OK;
}

void tbox_cat_free_messages(TBoxMessageRow *rows, size_t count) {
    if (rows == NULL) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        free(rows[i].text);
    }
    free(rows);
}
