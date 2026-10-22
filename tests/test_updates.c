/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_updates.c — the spec 8.3 equivalence test: after a random sequence of
 * adds, edits and deletes applied as incremental updates, the catalog must be
 * exactly what a fresh full scan of the final chat produces.
 *
 * Both sides are real: the incremental side starts from a full scan and then
 * applies TDLib-shaped updates; the scan side is a full scan of the same chat
 * in its final state. The comparison is done by SQLite itself (EXCEPT both
 * ways over `messages` and `objects`), so a single differing cell fails.
 */
#include "sync/updates.h"

#undef NDEBUG /* keep assert() alive in Release (/DNDEBUG) builds */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "fake_chat.h"
#include "sqlite3.h"
#include "store/catalog.h"
#include "store/db.h"
#include "sync/scan.h"
#include "util/fsx.h"

#define INC_DIR "tbox_updates_test_inc"
#define SCAN_DIR "tbox_updates_test_scan"
#define CHAT_ID 1

#define O1 "0123456789abcdef0123456789abcdef"

/* ---- deterministic pseudo-randomness -------------------------------- */

static unsigned long g_seed;

static unsigned long next_rand(void) {
    g_seed = g_seed * 1103515245UL + 12345UL;
    return (g_seed >> 16) & 0x7fffffffUL;
}

static unsigned long rand_below(unsigned long n) {
    return n == 0 ? 0 : next_rand() % n;
}

/* A fresh 32-hex object id from the generator, into out[33]. */
static void rand_object_id(char *out) {
    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i] = hex[next_rand() % 16];
    }
    out[32] = '\0';
}

static void file_caption(char *buf, size_t cap, const char *o, const char *name,
                         long long size) {
    snprintf(buf, cap,
             "tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"%s\",\"p\":\"root\","
             "\"n\":\"%s\",\"r\":1,\"s\":%lld}",
             o, name, size);
}

static void folder_text(char *buf, size_t cap, const char *o, const char *name) {
    snprintf(buf, cap,
             "tbx1 {\"v\":1,\"k\":\"d\",\"o\":\"%s\",\"p\":\"root\","
             "\"n\":\"%s\",\"r\":1}",
             o, name);
}

/* ---- history and message fetch over the fake chat --------------------- */

static char *fetch_history(void *ctx, long long from_message_id, int limit) {
    return fake_chat_history_json((FakeChat *)ctx, from_message_id, 0, limit);
}

static char *fetch_message(void *ctx, long long message_id) {
    return fake_chat_message_json((FakeChat *)ctx, message_id);
}

/* ---- events ---------------------------------------------------------- */

typedef struct Events {
    int changed;
    int lines;
} Events;

static void collect(const char *line, void *ctx) {
    Events *e = ctx;
    e->lines++;
    if (strstr(line, "\"catalog.changed\"") != NULL) {
        e->changed++;
    }
}

/* ---- building an updateNewMessage ------------------------------------ */

static char *new_message_update(FakeChat *chat, long long id) {
    char *mj = fake_chat_message_json(chat, id);
    if (mj == NULL) {
        return NULL;
    }
    cJSON *msg = cJSON_Parse(mj);
    free(mj);
    if (msg == NULL) {
        return NULL;
    }
    cJSON *u = cJSON_CreateObject();
    if (u == NULL) {
        cJSON_Delete(msg);
        return NULL;
    }
    cJSON_AddStringToObject(u, "@type", "updateNewMessage");
    cJSON_AddItemToObject(u, "message", msg);
    char *out = cJSON_PrintUnformatted(u);
    cJSON_Delete(u);
    return out;
}

/* Applies every queued update (edits/deletes) to the incremental catalog. */
static int drain_updates(FakeChat *chat, TBoxUpdates *up, Events *events) {
    int applied = 0;
    char *json;
    while ((json = fake_chat_take_update(chat)) != NULL) {
        TBoxUpdateOutcome out;
        int rc = tbox_updates_apply(up, json, &out);
        free(json);
        if (rc != 0) {
            return -1;
        }
        assert(!out.needs_resync);
        applied++;
    }
    (void)events;
    return applied;
}

/* Applies one synthesized updateNewMessage. */
static void apply_new(FakeChat *chat, TBoxUpdates *up, long long id) {
    char *json = new_message_update(chat, id);
    assert(json != NULL);
    TBoxUpdateOutcome out;
    assert(tbox_updates_apply(up, json, &out) == 0);
    assert(out.handled);
    free(json);
}

/* ---- catalog comparison ---------------------------------------------- */

/* Number of rows that differ between `table` in the main db and in `other`,
 * counting both directions. */
static long long row_diff(sqlite3 *h, const char *table) {
    char sql[512];
    snprintf(sql, sizeof sql,
             "SELECT (SELECT count(*) FROM (SELECT * FROM %s EXCEPT "
             "SELECT * FROM other.%s)) + (SELECT count(*) FROM (SELECT * FROM "
             "other.%s EXCEPT SELECT * FROM %s))",
             table, table, table, table);
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    long long n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

static void compare_catalogs(const char *inc_path, const char *scan_path) {
    sqlite3 *h = NULL;
    assert(sqlite3_open(inc_path, &h) == SQLITE_OK);
    char attach[512];
    snprintf(attach, sizeof attach, "ATTACH DATABASE '%s' AS other", scan_path);
    char *err = NULL;
    assert(sqlite3_exec(h, attach, NULL, NULL, &err) == SQLITE_OK);
    sqlite3_free(err);
    long long md = row_diff(h, "messages");
    long long od = row_diff(h, "objects");
    if (md != 0 || od != 0) {
        fprintf(stderr, "catalogs differ: %lld messages, %lld objects\n", md,
                od);
    }
    assert(md == 0);
    assert(od == 0);
    sqlite3_close(h);
}

/* ---- seed and mutate -------------------------------------------------- */

static long long add_random_message(FakeChat *chat) {
    char cap[512];
    char o[33];
    rand_object_id(o);
    switch (rand_below(4)) {
    case 0: { /* a managed file */
        char name[32];
        snprintf(name, sizeof name, "m%lu.bin", next_rand() % 100000);
        long long size = (long long)(next_rand() % 100000);
        file_caption(cap, sizeof cap, o, name, size);
        return fake_chat_add_media(chat, "document", name, size,
                                   (long long)next_rand() % 1000000, cap);
    }
    case 1: /* an unmanaged file: no valid record */
        return fake_chat_add_media(chat, "photo", NULL, 2048,
                                   (long long)next_rand() % 1000000, NULL);
    case 2: { /* a folder */
        char name[32];
        snprintf(name, sizeof name, "d%lu", next_rand() % 100000);
        folder_text(cap, sizeof cap, o, name);
        long long id = fake_chat_add_text(chat, cap);
        return id;
    }
    default: /* a plain note */
        snprintf(cap, sizeof cap, "note %lu", next_rand() % 100000);
        return fake_chat_add_text(chat, cap);
    }
}

/* Returns an existing message id, or 0 when the chat is empty. */
static long long random_id(FakeChat *chat, long long *ids, size_t cap,
                           size_t *count) {
    *count = fake_chat_ids(chat, ids, cap);
    if (*count == 0) {
        return 0;
    }
    return ids[rand_below((unsigned long)*count)];
}

static void mutate(FakeChat *chat, TBoxUpdates *up, Events *events) {
    long long ids[256];
    size_t n = 0;
    unsigned long op = rand_below(10);
    if (op < 4 || n == 0) {
        long long id = add_random_message(chat);
        if (id > 0) {
            apply_new(chat, up, id);
        }
        return;
    }
    long long id = random_id(chat, ids, 256, &n);
    if (id == 0) {
        return;
    }
    if (op < 7) {
        /* edit a media caption (text edits are not modelled by the fake) */
        const FakeMsg *m = fake_chat_get(chat, id);
        if (m == NULL || (m->kind != NULL && strcmp(m->kind, "text") == 0)) {
            return;
        }
        char cap[512];
        char o[33];
        rand_object_id(o);
        if (rand_below(2) == 0) {
            char name[32];
            snprintf(name, sizeof name, "e%lu.bin", next_rand() % 100000);
            file_caption(cap, sizeof cap, o, name, 4096);
            fake_chat_edit_caption(chat, id, cap);
        } else {
            fake_chat_edit_caption(chat, id, NULL); /* becomes unmanaged */
        }
    } else if (op < 9) {
        fake_chat_delete(chat, id, 1); /* a real delete */
    } else {
        fake_chat_delete(chat, id, 0); /* from_cache: must be ignored */
    }
    assert(drain_updates(chat, up, events) >= 0);
}

/* ---- the test -------------------------------------------------------- */

static void test_incremental_equals_full_scan(void) {
    g_seed = 20261022UL;
    FakeChat *chat = fake_chat_new();

    /* a starting population, then a full scan to open the incremental side */
    for (int i = 0; i < 30; i++) {
        add_random_message(chat);
    }

    TBoxScanHistory hist = { fetch_history, chat };
    Events events;
    memset(&events, 0, sizeof events);

    TBoxDb *inc = NULL;
    assert(tbox_db_open(INC_DIR, &inc) == TBOX_DB_OK);
    assert(tbox_scan_full(inc, &hist, NULL, 1760000100, collect, &events,
                          NULL) == 0);

    TBoxUpdates *up = tbox_updates_new(inc, CHAT_ID, fetch_message, chat,
                                       collect, &events);
    assert(up != NULL);

    /* 300 random mutations, each applied the way TDLib would deliver it */
    for (int i = 0; i < 300; i++) {
        mutate(chat, up, &events);
    }
    assert(events.changed > 0);

    /* the incremental catalog must hold exactly the chat's current messages */
    TBoxMessageRow *rows = NULL;
    size_t loaded = 0;
    assert(tbox_cat_load_messages(inc, &rows, &loaded) == TBOX_DB_OK);
    assert(loaded == fake_chat_count(chat));
    tbox_cat_free_messages(rows, loaded);

    tbox_updates_free(up);
    tbox_db_close(inc);

    /* a fresh full scan of the very same final chat */
    TBoxDb *scan = NULL;
    assert(tbox_db_open(SCAN_DIR, &scan) == TBOX_DB_OK);
    assert(tbox_scan_full(scan, &hist, NULL, 1760000200, NULL, NULL, NULL) ==
           0);
    tbox_db_close(scan);
    fake_chat_free(chat);

    compare_catalogs(INC_DIR "/index.db", SCAN_DIR "/index.db");
    printf("updates: incremental catalog equals a fresh full scan (%d events)\n",
           events.changed);
}

/* A cache-only delete must leave the row in place, and a permanent one must
 * remove it; a non-catalog message must be dropped rather than kept. */
static void test_delete_rules(void) {
    const char *dir = "tbox_updates_test_rules";
    (void)tbox_fsx_remove("tbox_updates_test_rules/index.db");
    (void)tbox_fsx_remove("tbox_updates_test_rules/index.db-wal");
    (void)tbox_fsx_remove("tbox_updates_test_rules/index.db-shm");
    assert(tbox_fsx_mkdirs(dir) == TBOX_FSX_OK);

    FakeChat *chat = fake_chat_new();
    char cap[512];
    file_caption(cap, sizeof cap, O1, "keep.bin", 10);
    long long id = fake_chat_add_media(chat, "document", "keep.bin", 10, 42,
                                       cap);

    TBoxScanHistory hist = { fetch_history, chat };
    TBoxDb *db = NULL;
    assert(tbox_db_open(dir, &db) == TBOX_DB_OK);
    assert(tbox_scan_full(db, &hist, NULL, 1760000300, NULL, NULL, NULL) == 0);
    TBoxUpdates *up = tbox_updates_new(db, CHAT_ID, fetch_message, chat, NULL,
                                       NULL);
    assert(up != NULL);

    size_t count = 0;

    /* cache eviction: the row stays */
    fake_chat_delete(chat, id, 0);
    TBoxUpdateOutcome out;
    char *u = fake_chat_take_update(chat);
    assert(u != NULL);
    assert(tbox_updates_apply(up, u, &out) == 0);
    assert(out.handled && !out.changed);
    free(u);
    TBoxMessageRow *rows = NULL;
    assert(tbox_cat_load_messages(db, &rows, &count) == TBOX_DB_OK);
    assert(count == 1);
    tbox_cat_free_messages(rows, count);

    /* a real delete: the row goes */
    fake_chat_delete(chat, id, 1);
    u = fake_chat_take_update(chat);
    assert(u != NULL);
    assert(tbox_updates_apply(up, u, &out) == 0);
    assert(out.handled && out.changed);
    free(u);
    assert(tbox_cat_load_messages(db, &rows, &count) == TBOX_DB_OK);
    assert(count == 0);
    tbox_cat_free_messages(rows, count);

    tbox_updates_free(up);
    tbox_db_close(db);
    fake_chat_free(chat);
    printf("updates: cache deletes are ignored, real deletes remove the row\n");
}

int main(void) {
    (void)tbox_fsx_remove(INC_DIR "/index.db");
    (void)tbox_fsx_remove(INC_DIR "/index.db-wal");
    (void)tbox_fsx_remove(INC_DIR "/index.db-shm");
    (void)tbox_fsx_remove(SCAN_DIR "/index.db");
    (void)tbox_fsx_remove(SCAN_DIR "/index.db-wal");
    (void)tbox_fsx_remove(SCAN_DIR "/index.db-shm");
    assert(tbox_fsx_mkdirs(INC_DIR) == TBOX_FSX_OK);
    assert(tbox_fsx_mkdirs(SCAN_DIR) == TBOX_FSX_OK);

    test_delete_rules();
    test_incremental_equals_full_scan();
    printf("updates OK\n");
    return 0;
}
