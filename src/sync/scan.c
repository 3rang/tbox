/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * scan.c — the full scan of spec 8.2 (see scan.h).
 *
 * The scan hands resolve the messages it saw, so it keeps them alive in two
 * parallel arrays as it pages: the `messages` rows it wrote, and the narrowed
 * TBoxResolveMsg views that point into those rows. Nothing is freed until the
 * whole scan finishes, which is also what makes an interrupted scan safe:
 * because deletion and the object rebuild happen only after the final empty
 * page, a failure anywhere before that leaves the catalog exactly as it was.
 */
#include "sync/scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "catalog/message.h"
#include "catalog/resolve.h"
#include "store/catalog.h"
#include "store/db.h"

/* Spec 8.2 asks for pages of this size. */
#define SCAN_PAGE_LIMIT 100

#ifdef _WIN32
#define TBOX_STRDUP _strdup
#else
#define TBOX_STRDUP strdup
#endif

/* ---- the growing set of what the scan saw ---------------------------- */

typedef struct SeenSet {
    char **ids;
    size_t count;
    size_t cap;
} SeenSet;

typedef struct MsgList {
    TBoxMessageRow  **rows;    /* individually allocated: stable addresses */
    TBoxResolveMsg   *views;   /* point into the rows above */
    size_t            count;
    size_t            row_cap;
    size_t            view_cap;
} MsgList;

typedef struct Scan {
    TBoxDb            *db;
    const char        *op_id;
    long long          now_unix;
    TBoxScanEmitFn     emit;
    void              *emit_ctx;
    SeenSet            seen;
    MsgList            msgs;
    int                failed;
} Scan;

/* Grows a buffer to at least `needed` elements, doubling. Returns 0 on
 * success, -1 on out-of-memory. */
static int grow(void **buf, size_t *cap, size_t needed, size_t elem) {
    if (*cap >= needed) {
        return 0;
    }
    size_t ncap = *cap == 0 ? 16 : *cap;
    while (ncap < needed) {
        ncap *= 2;
    }
    void *grown = realloc(*buf, ncap * elem);
    if (grown == NULL) {
        return -1;
    }
    *buf = grown;
    *cap = ncap;
    return 0;
}

/* Remembers a message id, or reports out-of-memory. */
static int seen_add(SeenSet *s, const char *id) {
    if (grow((void **)&s->ids, &s->cap, s->count + 1, sizeof *s->ids) != 0) {
        return -1;
    }
    char *copy = TBOX_STRDUP(id);
    if (copy == NULL) {
        return -1;
    }
    s->ids[s->count++] = copy;
    return 0;
}

static int seen_has(const SeenSet *s, const char *id) {
    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->ids[i], id) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Appends one parsed message, taking a heap copy of the row (its address must
 * stay put, because the view points into it) and a matching resolve view. */
static int msgs_add(MsgList *l, const TBoxMessageRow *row) {
    if (grow((void **)&l->rows, &l->row_cap, l->count + 1,
             sizeof *l->rows) != 0 ||
        grow((void **)&l->views, &l->view_cap, l->count + 1,
             sizeof *l->views) != 0) {
        return -1;
    }
    TBoxMessageRow *dst = malloc(sizeof *dst);
    if (dst == NULL) {
        return -1;
    }
    *dst = *row;
    dst->text = NULL; /* ownership of the text stays with the caller's row */
    l->rows[l->count] = dst;

    TBoxResolveMsg *v = &l->views[l->count];
    memset(v, 0, sizeof *v);
    v->message_id = dst->message_id;
    v->date = dst->date;
    v->orig_name = dst->orig_name[0] != '\0' ? dst->orig_name : NULL;
    v->mime = dst->mime[0] != '\0' ? dst->mime : NULL;
    v->has_size = dst->has_size;
    v->size = dst->size;
    v->bad_record = dst->bad_record;
    if (strcmp(dst->kind, TBOX_MSG_KIND_FILE) == 0) {
        v->kind = TBOX_MSG_FILE;
        v->rec = dst->rec;
    } else if (strcmp(dst->kind, TBOX_MSG_KIND_FOLDER) == 0) {
        v->kind = TBOX_MSG_FOLDER;
        v->rec = dst->rec;
    } else if (strcmp(dst->kind, TBOX_MSG_KIND_UNMANAGED) == 0) {
        v->kind = TBOX_MSG_UNMANAGED;
    } else {
        v->kind = TBOX_MSG_NOTE;
    }
    l->count++;
    return 0;
}

static void msgs_free(MsgList *l) {
    for (size_t i = 0; i < l->count; i++) {
        tbox_message_row_free(l->rows[i]);
        free(l->rows[i]);
    }
    free(l->rows);
    free(l->views);
    l->rows = NULL;
    l->views = NULL;
    l->count = 0;
    l->row_cap = 0;
    l->view_cap = 0;
}

static void seen_free(SeenSet *s) {
    for (size_t i = 0; i < s->count; i++) {
        free(s->ids[i]);
    }
    free(s->ids);
    s->ids = NULL;
    s->count = 0;
    s->cap = 0;
}

/* ---- events ---------------------------------------------------------- */

/* Emits one event envelope: {"event":<name>,"data":<data>}, where `data` is
 * built by the caller as a JSON object. `data` may be NULL for no payload. */
static void emit_event(const Scan *s, const char *name, cJSON *data) {
    if (s->emit == NULL) {
        if (data != NULL) {
            cJSON_Delete(data);
        }
        return;
    }
    cJSON *env = cJSON_CreateObject();
    if (env == NULL) {
        if (data != NULL) {
            cJSON_Delete(data);
        }
        return;
    }
    cJSON_AddStringToObject(env, "event", name);
    if (data != NULL) {
        cJSON_AddItemToObject(env, "data", data);
    } else {
        cJSON_AddObjectToObject(env, "data");
    }
    char *line = cJSON_PrintUnformatted(env);
    cJSON_Delete(env);
    if (line != NULL) {
        s->emit(line, s->emit_ctx);
        free(line);
    }
}

static void emit_progress(const Scan *s, long long scanned) {
    cJSON *d = cJSON_CreateObject();
    if (d == NULL) {
        return;
    }
    if (s->op_id != NULL) {
        cJSON_AddStringToObject(d, "op_id", s->op_id);
    }
    cJSON_AddNumberToObject(d, "messages_scanned", (double)scanned);
    emit_event(s, "sync.progress", d);
}

static void emit_done(const Scan *s, long long objects, long long inbox_items) {
    cJSON *d = cJSON_CreateObject();
    if (d == NULL) {
        return;
    }
    if (s->op_id != NULL) {
        cJSON_AddStringToObject(d, "op_id", s->op_id);
    }
    cJSON_AddNumberToObject(d, "objects", (double)objects);
    cJSON_AddNumberToObject(d, "inbox_items", (double)inbox_items);
    emit_event(s, "sync.done", d);
}

static void emit_changed(const Scan *s) {
    cJSON *d = cJSON_CreateObject();
    if (d == NULL) {
        return;
    }
    cJSON *ids = cJSON_AddArrayToObject(d, "folder_ids");
    if (ids != NULL) {
        cJSON_AddItemToArray(ids, cJSON_CreateString("*"));
    }
    emit_event(s, "catalog.changed", d);
}

/* ---- one page -------------------------------------------------------- */

/* Writes one history page (in a transaction) and remembers what it saw.
 * Returns 1 when the page held messages, 0 when it was empty (history ended),
 * and -1 on a write failure. `*smallest` is the lowest message id on the page.
 */
static int write_page(Scan *s, const cJSON *messages, long long *smallest) {
    int n = cJSON_GetArraySize(messages);
    if (n == 0) {
        return 0;
    }
    if (tbox_cat_begin(s->db) != TBOX_DB_OK) {
        return -1;
    }
    int fail = 0;
    long long low = 0;
    int have_low = 0;
    for (int i = 0; i < n; i++) {
        const cJSON *m = cJSON_GetArrayItem(messages, i);
        TBoxMessageRow row;
        int parsed = tbox_message_parse(m, &row);
        if (parsed < 0) {
            continue; /* a message we cannot even read is not catalog data */
        }
        long long id = strtoll(row.message_id, NULL, 10);
        if (!have_low || id < low) {
            low = id;
            have_low = 1;
        }
        if (parsed == 0) {
            continue; /* a sticker, a poll, ...: ignored but still paged past */
        }
        if (seen_add(&s->seen, row.message_id) != 0 ||
            msgs_add(&s->msgs, &row) != 0 ||
            tbox_cat_upsert_message(s->db, &row) != TBOX_DB_OK) {
            tbox_message_row_free(&row);
            fail = 1;
            break;
        }
        tbox_message_row_free(&row);
    }
    if (fail) {
        (void)tbox_cat_rollback(s->db);
        return -1;
    }
    if (tbox_cat_commit(s->db) != TBOX_DB_OK) {
        return -1;
    }
    *smallest = low;
    return 1;
}

/* ---- the scan -------------------------------------------------------- */

int tbox_scan_full(TBoxDb *db, const TBoxScanHistory *history,
                   const char *op_id, long long now_unix,
                   TBoxScanEmitFn emit, void *emit_ctx, TBoxScanStats *stats) {
    if (db == NULL || history == NULL || history->fetch == NULL) {
        return -1;
    }
    Scan s;
    memset(&s, 0, sizeof s);
    s.db = db;
    s.op_id = op_id;
    s.now_unix = now_unix;
    s.emit = emit;
    s.emit_ctx = emit_ctx;

    long long from = 0;
    long long scanned = 0;
    for (;;) {
        char *page = history->fetch(history->ctx, from, SCAN_PAGE_LIMIT);
        if (page == NULL) {
            s.failed = 1; /* no page: fail without touching the catalog */
            break;
        }
        cJSON *root = cJSON_Parse(page);
        free(page);
        if (root == NULL) {
            s.failed = 1;
            break;
        }
        const cJSON *messages = cJSON_GetObjectItemCaseSensitive(root,
                                                                 "messages");
        long long smallest = 0;
        int rc = write_page(&s, messages, &smallest);
        scanned += (long long)cJSON_GetArraySize(messages);
        cJSON_Delete(root);
        if (rc < 0) {
            s.failed = 1;
            break;
        }
        if (rc == 0) {
            break; /* an empty page is the end of history */
        }
        emit_progress(&s, scanned);
        from = smallest;
    }

    if (s.failed) {
        msgs_free(&s.msgs);
        seen_free(&s.seen);
        return -1;
    }

    /* History ended. Now - and only now - the catalog may lose rows: the ones
     * that were in `messages` but were not seen in this scan. */
    long long deleted = 0;
    char **existing = NULL;
    size_t existing_n = 0;
    if (tbox_cat_list_message_ids(s.db, &existing, &existing_n) == TBOX_DB_OK) {
        if (tbox_cat_begin(s.db) == TBOX_DB_OK) {
            for (size_t i = 0; i < existing_n; i++) {
                if (!seen_has(&s.seen, existing[i])) {
                    if (tbox_cat_delete_messages(s.db, (const char *const *)&existing[i], 1) == TBOX_DB_OK) {
                        deleted++;
                    }
                }
            }
            (void)tbox_cat_commit(s.db);
        }
        tbox_cat_free_ids(existing, existing_n);
    }

    /* Rebuild the resolved view from exactly what this scan saw. */
    TBoxResolveResult res;
    memset(&res, 0, sizeof res);
    long long objects = 0;
    long long inbox = 0;
    if (tbox_resolve(s.msgs.views, s.msgs.count, &res) == 0) {
        objects = (long long)res.object_count;
        inbox = (long long)res.inbox_count;
        (void)tbox_cat_replace_objects(s.db, &res);
        tbox_resolve_result_free(&res);
    }

    char stamp[32];
    snprintf(stamp, sizeof stamp, "%lld", s.now_unix);
    (void)tbox_db_meta_set(s.db, TBOX_DB_META_SCAN_COMPLETE, "1");
    (void)tbox_db_meta_set(s.db, TBOX_DB_META_LAST_FULL_SCAN, stamp);

    emit_done(&s, objects, inbox);
    emit_changed(&s);

    if (stats != NULL) {
        stats->messages_scanned = scanned;
        stats->objects = objects;
        stats->inbox_items = inbox;
        stats->deleted = deleted;
    }
    msgs_free(&s.msgs);
    seen_free(&s.seen);
    return 0;
}
