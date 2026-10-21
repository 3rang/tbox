/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * catalog.c — the catalog store (see catalog.h). Every statement is prepared
 * once per call, every value is bound, and the connection is only ever the
 * engine's own (spec 3.1 rule 2).
 */
#include "store/catalog.h"

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
