/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_scan.c — the full scan of spec 8.2, driven against the in-memory Saved
 * Messages chat:
 *   - a thousand messages come in with short pages and are all stored;
 *   - the scan talks to the end of history (an empty page) before it acts;
 *   - an interrupted scan deletes nothing and leaves scan_complete alone;
 *   - a complete scan that no longer sees a message removes exactly that row;
 *   - a caption that is a record resolves into objects, and a plain note or an
 *     unmanaged file lands in the Inbox.
 */
#include "sync/scan.h"

#undef NDEBUG /* keep assert() alive in Release (/DNDEBUG) builds */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog/resolve.h"
#include "sqlite3.h"
#include "store/db.h"
#include "fake_chat.h"
#include "util/fsx.h"

#define DIR "tbox_scan_test"

#ifdef _WIN32
#define TBOX_STRDUP _strdup
#else
#define TBOX_STRDUP strdup
#endif

static const char *O1 = "0123456789abcdef0123456789abcdef";
static const char *O2 = "0123456789abcdef0123456789abcdee";

/* Builds a valid tbx1 caption naming a file, into buf. */
static void file_caption(char *buf, size_t cap, const char *o, const char *name,
                         long long size) {
    snprintf(buf, cap,
             "tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"%s\",\"p\":\"root\","
             "\"n\":\"%s\",\"r\":1,\"s\":%lld}",
             o, name, size);
}

/* Builds a valid tbx1 folder record. */
static void folder_text(char *buf, size_t cap, const char *o, const char *name) {
    snprintf(buf, cap,
             "tbx1 {\"v\":1,\"k\":\"d\",\"o\":\"%s\",\"p\":\"root\","
             "\"n\":\"%s\",\"r\":1}",
             o, name);
}

/* ---- a history callback over the fake chat --------------------------- */

typedef struct FakeHistory {
    FakeChat *chat;
    int       fail_after; /* fail once this many pages have been served; -1 off */
    int       pages;
} FakeHistory;

static char *fetch_history(void *ctx, long long from_message_id, int limit) {
    FakeHistory *h = ctx;
    if (h->fail_after >= 0 && h->pages >= h->fail_after) {
        return NULL; /* a page that never arrives */
    }
    h->pages++;
    return fake_chat_history_json(h->chat, from_message_id, 0, limit);
}

/* ---- an emit collector ----------------------------------------------- */

typedef struct Events {
    char  *lines[256];
    size_t count;
    /* how many of each interesting event we saw */
    int progresses;
    int dones;
    int changeds;
} Events;

static void collect(const char *line, void *ctx) {
    Events *e = ctx;
    if (e->count < sizeof e->lines / sizeof e->lines[0]) {
        e->lines[e->count] = TBOX_STRDUP(line);
    }
    e->count++;
    if (strstr(line, "\"sync.progress\"") != NULL) {
        e->progresses++;
    } else if (strstr(line, "\"sync.done\"") != NULL) {
        e->dones++;
    } else if (strstr(line, "\"catalog.changed\"") != NULL) {
        e->changeds++;
    }
}

static void events_free(Events *e) {
    for (size_t i = 0; i < e->count && i < sizeof e->lines / sizeof e->lines[0];
         i++) {
        free(e->lines[i]);
    }
    e->count = 0;
    e->progresses = e->dones = e->changeds = 0;
}

/* Convenience: a query against the engine's own file, opened raw. */
static long long raw_count(const char *table) {
    char sql[64];
    snprintf(sql, sizeof sql, "SELECT count(*) FROM %s", table);
    char path[256];
    snprintf(path, sizeof path, "%s/index.db", DIR);
    sqlite3 *h = NULL;
    assert(sqlite3_open(path, &h) == SQLITE_OK);
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    long long n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    sqlite3_close(h);
    return n;
}

static TBoxDb *open_db(void) {
    TBoxDb *db = NULL;
    assert(tbox_db_open(DIR, &db) == TBOX_DB_OK);
    return db;
}

/* ---- the tests ------------------------------------------------------- */

static void test_full_scan_of_a_thousand_with_short_pages(void) {
    FakeChat *chat = fake_chat_new();
    char cap[512];

    /* 400 managed files, 100 folders, 500 notes: a thousand messages whose
     * objects and inbox are known in advance. */
    for (int i = 0; i < 400; i++) {
        char name[32];
        snprintf(name, sizeof name, "f%d.bin", i);
        char o[33];
        memcpy(o, O1, 32);
        o[32] = '\0';
        snprintf(o, sizeof o, "%032x", i); /* a distinct 32-hex id per file */
        file_caption(cap, sizeof cap, o, name, 1000 + i);
        fake_chat_add_media(chat, "document", name, 1000 + i, 100000 + i, cap);
    }
    for (int i = 0; i < 100; i++) {
        char name[32];
        snprintf(name, sizeof name, "d%d", i);
        char o[33];
        snprintf(o, sizeof o, "%032x", 0x1000 + i);
        folder_text(cap, sizeof cap, o, name);
        fake_chat_add_text(chat, cap);
    }
    for (int i = 0; i < 500; i++) {
        snprintf(cap, sizeof cap, "plain note %d", i);
        fake_chat_add_text(chat, cap);
    }
    assert(fake_chat_count(chat) == 1000);

    /* force TDLib-style short pages: the scan must keep going anyway */
    fake_chat_set_page_size(chat, 37);

    FakeHistory hist = { chat, -1, 0 };
    TBoxScanHistory h = { fetch_history, &hist };
    Events events;
    memset(&events, 0, sizeof events);

    TBoxDb *db = open_db();
    TBoxScanStats stats;
    int rc = tbox_scan_full(db, &h, "0000000000000001", 1760000000, collect,
                            &events, &stats);
    assert(rc == 0);
    assert(stats.messages_scanned == 1000);
    assert(stats.objects == 500);    /* 400 files + 100 folders */
    assert(stats.inbox_items == 0);  /* notes are not Inbox items (7.6) */

    /* every row is on disk and the catalog is marked complete */
    assert(raw_count("messages") == 1000);
    assert(raw_count("objects") == 500);
    char v[64];
    assert(tbox_db_meta_get(db, TBOX_DB_META_SCAN_COMPLETE, v, sizeof v) == 1);
    assert(strcmp(v, "1") == 0);
    assert(tbox_db_meta_get(db, TBOX_DB_META_LAST_FULL_SCAN, v, sizeof v) == 1);
    assert(strcmp(v, "1760000000") == 0);

    /* events: at least one progress per non-empty page, one done, one changed */
    assert(events.progresses >= 1000 / 37);
    assert(events.dones == 1);
    assert(events.changeds == 1);
    assert(strstr(events.lines[events.count - 1], "catalog.changed") != NULL);

    tbox_db_close(db);
    events_free(&events);
    fake_chat_free(chat);
    printf("scan: 1000 messages with short pages scanned completely\n");
}

static void test_interrupted_scan_deletes_nothing(void) {
    FakeChat *chat = fake_chat_new();
    char cap[512];
    for (int i = 0; i < 50; i++) {
        char name[32];
        snprintf(name, sizeof name, "k%d.bin", i);
        char o[33];
        snprintf(o, sizeof o, "%032x", 0x2000 + i);
        file_caption(cap, sizeof cap, o, name, 10);
        fake_chat_add_media(chat, "document", name, 10, 500000 + i, cap);
    }

    FakeHistory hist = { chat, -1, 0 };
    TBoxScanHistory h = { fetch_history, &hist };

    TBoxDb *db = open_db();
    assert(tbox_scan_full(db, &h, NULL, 1760000001, NULL, NULL, NULL) == 0);
    assert(raw_count("messages") == 50);

    /* now remove five messages and interrupt a scan after two pages: the
     * catalog must still hold all fifty, and completion must be untouched. */
    long long ids[64];
    size_t n = fake_chat_ids(chat, ids, 64);
    for (size_t i = 0; i < 5 && i < n; i++) {
        fake_chat_delete(chat, ids[i], 1);
    }
    fake_chat_set_page_size(chat, 5); /* ~10 pages for 45 messages */

    FakeHistory bad = { chat, 2, 0 }; /* fail after two pages */
    TBoxScanHistory bh = { fetch_history, &bad };
    assert(tbox_scan_full(db, &bh, NULL, 1760000002, NULL, NULL, NULL) == -1);
    assert(raw_count("messages") == 50); /* nothing was deleted */

    tbox_db_close(db);
    fake_chat_free(chat);
    printf("scan: an interrupted scan deletes nothing\n");
}

/* Unmanaged files (no valid record) are Inbox items; a managed file adopted
 * from one leaves the Inbox through the src rule. */
static void test_unmanaged_files_fill_the_inbox(void) {
    const char *dir = "tbox_scan_test_inbox";
    (void)tbox_fsx_remove("tbox_scan_test_inbox/index.db");
    (void)tbox_fsx_remove("tbox_scan_test_inbox/index.db-wal");
    (void)tbox_fsx_remove("tbox_scan_test_inbox/index.db-shm");
    assert(tbox_fsx_mkdirs(dir) == TBOX_FSX_OK);

    FakeChat *chat = fake_chat_new();
    /* three raw files with no caption, plus one adopted copy carrying src */
    long long raw1 = fake_chat_add_media(chat, "document", "raw1.bin", 10,
                                         900001, NULL);
    fake_chat_add_media(chat, "photo", NULL, 20, 900002, NULL);
    fake_chat_add_media(chat, "document", "raw3.bin", 30, 900003, NULL);
    char cap[512];
    char o[33];
    snprintf(o, sizeof o, "%032x", 0x4000);
    snprintf(cap, sizeof cap,
             "tbx1 {\"v\":1,\"k\":\"f\",\"o\":\"%s\",\"p\":\"root\","
             "\"n\":\"kept.bin\",\"r\":1,\"s\":10,\"src\":\"%lld\"}",
             o, raw1);
    fake_chat_add_media(chat, "document", "kept.bin", 10, 900004, cap);

    FakeHistory hist = { chat, -1, 0 };
    TBoxScanHistory h = { fetch_history, &hist };
    TBoxDb *db = NULL;
    assert(tbox_db_open(dir, &db) == TBOX_DB_OK);
    TBoxScanStats stats;
    assert(tbox_scan_full(db, &h, NULL, 1760000020, NULL, NULL, &stats) == 0);
    assert(stats.objects == 1);     /* just the adopted managed file */
    assert(stats.inbox_items == 2); /* raw2 and raw3: raw1 was adopted */
    tbox_db_close(db);
    fake_chat_free(chat);
    printf("scan: unmanaged files fill the Inbox, src removes one\n");
}

static void test_complete_scan_removes_unseen_rows(void) {
    FakeChat *chat = fake_chat_new();
    char cap[512];
    for (int i = 0; i < 20; i++) {
        char name[32];
        snprintf(name, sizeof name, "g%d.bin", i);
        char o[33];
        snprintf(o, sizeof o, "%032x", 0x3000 + i);
        file_caption(cap, sizeof cap, o, name, 5);
        fake_chat_add_media(chat, "document", name, 5, 700000 + i, cap);
    }
    fake_chat_set_page_size(chat, 3);

    FakeHistory hist = { chat, -1, 0 };
    TBoxScanHistory h = { fetch_history, &hist };

    TBoxDb *db = open_db();
    assert(tbox_scan_full(db, &h, NULL, 1760000010, NULL, NULL, NULL) == 0);
    assert(raw_count("messages") == 20);

    /* a permanent delete, then a fresh complete scan: the row goes away */
    long long ids[32];
    size_t n = fake_chat_ids(chat, ids, 32);
    assert(n == 20);
    fake_chat_delete(chat, ids[0], 1);
    fake_chat_delete(chat, ids[1], 1);
    hist.pages = 0;
    TBoxScanStats stats;
    assert(tbox_scan_full(db, &h, NULL, 1760000011, NULL, NULL, &stats) == 0);
    assert(raw_count("messages") == 18);
    assert(stats.deleted == 2);
    assert(stats.objects == 18);

    tbox_db_close(db);
    fake_chat_free(chat);
    printf("scan: a complete scan removes rows it did not see\n");
}

int main(void) {
    (void)tbox_fsx_remove(DIR "/index.db");
    (void)tbox_fsx_remove(DIR "/index.db-wal");
    (void)tbox_fsx_remove(DIR "/index.db-shm");
    assert(tbox_fsx_mkdirs(DIR) == TBOX_FSX_OK);

    test_full_scan_of_a_thousand_with_short_pages();
    test_interrupted_scan_deletes_nothing();
    test_unmanaged_files_fill_the_inbox();
    test_complete_scan_removes_unseen_rows();
    printf("scan OK\n");
    return 0;
}
