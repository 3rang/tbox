/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * updates.c — incremental updates from Saved Messages (see updates.h).
 *
 * The catalog after a stream of updates must equal the catalog after a fresh
 * full scan of the same chat. The way this file guarantees that is blunt on
 * purpose: every change writes or deletes one `messages` row and then rebuilds
 * `objects` from the whole messages table. `objects` is derived data (spec
 * 7.5), so rebuilding it is the same code path a scan uses, and there is no
 * separate incremental resolver to drift out of agreement.
 */
#include "sync/updates.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "catalog/message.h"
#include "catalog/resolve.h"
#include "store/catalog.h"
#include "store/db.h"

#ifdef _WIN32
#define TBOX_STRDUP _strdup
#else
#define TBOX_STRDUP strdup
#endif

struct TBoxUpdates {
    TBoxDb           *db;
    long long         chat_id;
    TBoxUpdateFetchFn fetch;
    void             *fetch_ctx;
    TBoxUpdateEmitFn  emit;
    void             *emit_ctx;
};

/* ---- small JSON readers ---------------------------------------------- */

static const cJSON *field(const cJSON *o, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(o, key);
}

static const char *str_field(const cJSON *o, const char *key) {
    const cJSON *v = field(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int num_field(const cJSON *o, const char *key, long long *out) {
    const cJSON *v = field(o, key);
    if (!cJSON_IsNumber(v)) {
        return 0;
    }
    *out = (long long)v->valuedouble;
    return 1;
}

/* Returns 1 when this update belongs to the configured chat. An update with
 * no chat id at all is not a chat update and is not ours. */
static int for_our_chat(const TBoxUpdates *u, const cJSON *update) {
    long long chat = 0;
    if (num_field(update, "chat_id", &chat)) {
        return chat == u->chat_id;
    }
    const cJSON *msg = field(update, "message");
    if (cJSON_IsObject(msg) && num_field(msg, "chat_id", &chat)) {
        return chat == u->chat_id;
    }
    return 0;
}

/* ---- events ---------------------------------------------------------- */

static void emit_changed(TBoxUpdates *u) {
    if (u->emit == NULL) {
        return;
    }
    cJSON *env = cJSON_CreateObject();
    if (env == NULL) {
        return;
    }
    cJSON_AddStringToObject(env, "event", "catalog.changed");
    cJSON *data = cJSON_AddObjectToObject(env, "data");
    if (data != NULL) {
        cJSON *ids = cJSON_AddArrayToObject(data, "folder_ids");
        if (ids != NULL) {
            cJSON_AddItemToArray(ids, cJSON_CreateString("*"));
        }
    }
    char *line = cJSON_PrintUnformatted(env);
    cJSON_Delete(env);
    if (line != NULL) {
        u->emit(line, u->emit_ctx);
        free(line);
    }
}

/* ---- rebuild objects ------------------------------------------------- */

/* Fills a resolve view from a loaded message row. The view borrows the row's
 * stable buffers, so the rows array must outlive it. */
static void view_from_row(const TBoxMessageRow *r, TBoxResolveMsg *v) {
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

int tbox_updates_reresolve(TBoxUpdates *u, int emit_changed_flag) {
    if (u == NULL) {
        return -1;
    }
    TBoxMessageRow *rows = NULL;
    size_t count = 0;
    if (tbox_cat_load_messages(u->db, &rows, &count) != TBOX_DB_OK) {
        return -1;
    }
    TBoxResolveMsg *views = NULL;
    if (count > 0) {
        views = calloc(count, sizeof *views);
        if (views == NULL) {
            tbox_cat_free_messages(rows, count);
            return -1;
        }
        for (size_t i = 0; i < count; i++) {
            view_from_row(&rows[i], &views[i]);
        }
    }

    TBoxResolveResult res;
    memset(&res, 0, sizeof res);
    int ok = 0;
    if (tbox_resolve(views, count, &res) == 0) {
        ok = tbox_cat_replace_objects(u->db, &res) == TBOX_DB_OK;
        tbox_resolve_result_free(&res);
    }
    free(views);
    tbox_cat_free_messages(rows, count);
    if (!ok) {
        return -1;
    }
    if (emit_changed_flag) {
        emit_changed(u);
    }
    return 0;
}

/* ---- applying a message ---------------------------------------------- */

/* Deletes the row for one message id, if present. */
static int delete_one(TBoxUpdates *u, const char *message_id) {
    const char *ids[1] = { message_id };
    return tbox_cat_delete_messages(u->db, ids, 1) == TBOX_DB_OK ? 0 : -1;
}/* Writes one parsed message row (or removes it when it stopped being catalog
 * data). *touched is set when the row actually changed the table. */
static int apply_message(TBoxUpdates *u, const cJSON *message, int *touched) {
    TBoxMessageRow row;
    int parsed = tbox_message_parse(message, &row);
    if (parsed < 0) {
        return 0; /* unreadable message: nothing to do */
    }
    if (parsed == 0) {
        /* Not a catalog message. If a row exists for it (content changed to a
         * sticker, say), drop it so the catalog matches a fresh scan. A sticker
         * that was never catalog data touches nothing. */
        if (!tbox_cat_has_message(u->db, row.message_id)) {
            return 0;
        }
        if (delete_one(u, row.message_id) != 0) {
            return -1;
        }
        *touched = 1;
        return 0;
    }
    int rc = tbox_cat_upsert_message(u->db, &row) == TBOX_DB_OK ? 0 : -1;
    tbox_message_row_free(&row);
    if (rc == 0) {
        *touched = 1;
    }
    return rc;
}

/* ---- applying whole updates ------------------------------------------ */

static int apply_new_message(TBoxUpdates *u, const cJSON *update,
                             int *touched) {
    const cJSON *msg = field(update, "message");
    if (!cJSON_IsObject(msg)) {
        return 0;
    }
    return apply_message(u, msg, touched);
}

static int apply_edit(TBoxUpdates *u, const cJSON *update, int *touched,
                      int *needs_resync) {
    long long id = 0;
    if (!num_field(update, "message_id", &id)) {
        return 0;
    }
    if (u->fetch == NULL) {
        *needs_resync = 1;
        return 0;
    }
    char *json = u->fetch(u->fetch_ctx, id);
    if (json == NULL) {
        /* The message may have been deleted, or may not be readable yet. Do
         * not guess: ask the engine for a reconcile rather than dropping it. */
        *needs_resync = 1;
        return 0;
    }
    cJSON *msg = cJSON_Parse(json);
    free(json);
    if (msg == NULL) {
        *needs_resync = 1;
        return 0;
    }
    int rc = apply_message(u, msg, touched);
    cJSON_Delete(msg);
    return rc;
}

/* Returns 1 when the value is true (boolean or 1). */
static int is_true(const cJSON *o, const char *key) {
    const cJSON *v = field(o, key);
    if (v == NULL) {
        return 0;
    }
    if (cJSON_IsBool(v)) {
        return cJSON_IsTrue(v);
    }
    if (cJSON_IsNumber(v)) {
        return v->valuedouble != 0.0;
    }
    return 0;
}

static int apply_delete(TBoxUpdates *u, const cJSON *update, int *touched) {
    int from_cache = is_true(update, "from_cache");
    int permanent = is_true(update, "is_permanent");
    if (from_cache || !permanent) {
        return 0; /* a local cache eviction, not a real delete (8.3) */
    }
    const cJSON *ids = field(update, "message_ids");
    if (!cJSON_IsArray(ids)) {
        return 0;
    }
    /* Collect the ids as decimal strings first: the messages table keys on
     * the text form, which is also what the scan writes. */
    int n = cJSON_GetArraySize(ids);
    char **text = calloc((size_t)(n > 0 ? n : 1), sizeof *text);
    if (text == NULL) {
        return -1;
    }
    int made = 0;
    int rc = 0;
    for (int i = 0; i < n; i++) {
        const cJSON *v = cJSON_GetArrayItem(ids, i);
        if (!cJSON_IsNumber(v)) {
            continue;
        }
        char buf[TBOX_MSGID_MAX_BYTES + 1];
        snprintf(buf, sizeof buf, "%lld", (long long)v->valuedouble);
        text[made] = TBOX_STRDUP(buf);
        if (text[made] == NULL) {
            rc = -1;
            break;
        }
        made++;
    }
    if (rc == 0) {
        if (tbox_cat_begin(u->db) != TBOX_DB_OK) {
            rc = -1;
        } else {
            if (tbox_cat_delete_messages(u->db, (const char *const *)text,
                                         (size_t)made) != TBOX_DB_OK) {
                (void)tbox_cat_rollback(u->db);
                rc = -1;
            } else if (tbox_cat_commit(u->db) != TBOX_DB_OK) {
                rc = -1;
            }
        }
    }
    if (rc == 0 && made > 0) {
        *touched = 1;
    }
    for (int i = 0; i < made; i++) {
        free(text[i]);
    }
    free(text);
    return rc;
}

static int apply_send_succeeded(TBoxUpdates *u, const cJSON *update,
                                int *touched) {
    const cJSON *msg = field(update, "message");
    long long old_id = 0;
    long long new_id = 0;
    if (cJSON_IsObject(msg)) {
        int rc = apply_message(u, msg, touched);
        if (rc != 0) {
            return rc;
        }
        if (!num_field(msg, "id", &new_id)) {
            return 0;
        }
    }
    if (num_field(update, "old_message_id", &old_id) && new_id != 0) {
        char old_buf[TBOX_MSGID_MAX_BYTES + 1];
        char new_buf[TBOX_MSGID_MAX_BYTES + 1];
        snprintf(old_buf, sizeof old_buf, "%lld", old_id);
        snprintf(new_buf, sizeof new_buf, "%lld", new_id);
        if (tbox_cat_remap_temp_message(u->db, old_buf, new_buf) !=
            TBOX_DB_OK) {
            return -1;
        }
    }
    return 0;
}

int tbox_updates_apply(TBoxUpdates *u, const char *update_json,
                       TBoxUpdateOutcome *out) {
    if (u == NULL || update_json == NULL) {
        return -1;
    }
    TBoxUpdateOutcome local;
    memset(&local, 0, sizeof local);
    cJSON *update = cJSON_Parse(update_json);
    if (update == NULL || !cJSON_IsObject(update)) {
        cJSON_Delete(update);
        return -1;
    }
    const char *type = str_field(update, "@type");
    int touched = 0;
    int rc = 0;
    if (type != NULL) {
        if (strcmp(type, "updateNewMessage") == 0) {
            local.handled = 1;
            if (for_our_chat(u, update)) {
                rc = apply_new_message(u, update, &touched);
            }
        } else if (strcmp(type, "updateMessageContent") == 0 ||
                   strcmp(type, "updateMessageEdited") == 0) {
            local.handled = 1;
            if (for_our_chat(u, update)) {
                rc = apply_edit(u, update, &touched, &local.needs_resync);
            }
        } else if (strcmp(type, "updateDeleteMessages") == 0) {
            local.handled = 1;
            if (for_our_chat(u, update)) {
                rc = apply_delete(u, update, &touched);
            }
        } else if (strcmp(type, "updateMessageSendSucceeded") == 0) {
            local.handled = 1;
            if (for_our_chat(u, update)) {
                rc = apply_send_succeeded(u, update, &touched);
            }
        }
        /* updateMessageSendFailed and updateFile are Phase 2 concerns; other
         * update types simply do not concern the catalog. */
    }
    cJSON_Delete(update);
    if (rc != 0) {
        return -1;
    }
    if (touched) {
        if (tbox_updates_reresolve(u, 1) != 0) {
            return -1;
        }
        local.changed = 1;
    }
    if (out != NULL) {
        *out = local;
    }
    return 0;
}

/* ---- lifecycle ------------------------------------------------------- */

TBoxUpdates *tbox_updates_new(TBoxDb *db, long long chat_id,
                              TBoxUpdateFetchFn fetch, void *fetch_ctx,
                              TBoxUpdateEmitFn emit, void *emit_ctx) {
    if (db == NULL) {
        return NULL;
    }
    TBoxUpdates *u = calloc(1, sizeof *u);
    if (u == NULL) {
        return NULL;
    }
    u->db = db;
    u->chat_id = chat_id;
    u->fetch = fetch;
    u->fetch_ctx = fetch_ctx;
    u->emit = emit;
    u->emit_ctx = emit_ctx;
    return u;
}

void tbox_updates_free(TBoxUpdates *u) {
    free(u);
}
