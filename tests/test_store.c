/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_store.c — index.db: opening a fresh file runs the migrations, the
 * spec 7.5 pragmas are set, meta reads and writes, reopening is a no-op,
 * and a file written by a newer build is refused rather than downgraded.
 */
#include "store/db.h"

#undef NDEBUG /* keep assert() alive in Release (/DNDEBUG) builds */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite3.h"
#include "util/fsx.h"

#define DIR "tbox_store_test"

/* Opens the same file with a raw SQLite handle, for checks the public API
 * deliberately does not expose (a pragma, a row). */
static sqlite3 *raw_open(const char *path) {
    sqlite3 *h = NULL;
    assert(sqlite3_open(path, &h) == SQLITE_OK);
    return h;
}

static void raw_scalar(sqlite3 *h, const char *sql, char *out, size_t cap) {
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    const unsigned char *v = sqlite3_column_text(st, 0);
    snprintf(out, cap, "%s", v != NULL ? (const char *)v : "");
    sqlite3_finalize(st);
}

/* 1 when a table of this name exists in the main schema. */
static int table_exists(sqlite3 *h, const char *name) {
    sqlite3_stmt *st = NULL;
    assert(sqlite3_prepare_v2(
               h, "SELECT count(*) FROM sqlite_master WHERE type='table' AND name=?1",
               -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
    assert(sqlite3_step(st) == SQLITE_ROW);
    int found = sqlite3_column_int(st, 0) > 0;
    sqlite3_finalize(st);
    return found;
}

static void test_migration_from_empty(void) {
    TBoxDb *db = NULL;
    assert(tbox_db_open(DIR, &db) == TBOX_DB_OK);
    assert(db != NULL);

    /* the schema version this build writes is recorded on disk */
    assert(tbox_db_schema_version(db) == TBOX_DB_SCHEMA_VERSION);
    char v[32];
    assert(tbox_db_meta_get(db, TBOX_DB_META_SCHEMA_VERSION, v, sizeof v) == 1);
    assert(strcmp(v, "2") == 0);

    /* a key that was never set is simply absent, not an error */
    assert(tbox_db_meta_get(db, "nope", v, sizeof v) == 0);

    /* the spec 7.5 pragmas are set on the engine's own connection (these
     * are per-connection, so they are checked before it is closed) */
    assert(tbox_db_pragma_int(db, "synchronous") == 1); /* NORMAL */
    assert(tbox_db_pragma_int(db, "foreign_keys") == 1);
    assert(tbox_db_pragma_int(db, "journal_mode") != -1);
    /* the allow-list refuses anything else */
    assert(tbox_db_pragma_int(db, "user_version; DROP TABLE meta") == -1);

    tbox_db_close(db);

    /* the pragmas really landed (spec 7.5): WAL + foreign_keys on. journal
     * mode is persisted in the file; foreign_keys defaults to ON through
     * SQLITE_DEFAULT_FOREIGN_KEYS. synchronous is per-connection, so it is
     * asserted in test_migration_from_empty before the handle is closed
     * (a fresh raw connection would only show the SQLite default). */
    char path[256];
    snprintf(path, sizeof path, "%s/index.db", DIR);
    sqlite3 *h = raw_open(path);
    raw_scalar(h, "PRAGMA journal_mode", v, sizeof v);
    assert(strcmp(v, "wal") == 0);
    raw_scalar(h, "PRAGMA foreign_keys", v, sizeof v);
    assert(strcmp(v, "1") == 0);

    /* v2 created the spec 7.5 catalog tables (no FTS yet) */
    assert(table_exists(h, "meta"));
    assert(table_exists(h, "messages"));
    assert(table_exists(h, "objects"));
    assert(table_exists(h, "ops"));
    assert(!table_exists(h, "search_fts"));
    sqlite3_close(h);

    printf("store: fresh file migrates to v%d with WAL + foreign_keys\n",
           TBOX_DB_SCHEMA_VERSION);
}

static void test_reopen_is_idempotent(void) {
    TBoxDb *db = NULL;
    assert(tbox_db_open(DIR, &db) == TBOX_DB_OK);
    assert(tbox_db_meta_set(db, TBOX_DB_META_DB_KEY_SET, "1") == TBOX_DB_OK);
    tbox_db_close(db);

    /* second open must not re-run migration 1 (which would fail: the table
     * already exists) and must keep the meta row */
    TBoxDb *again = NULL;
    assert(tbox_db_open(DIR, &again) == TBOX_DB_OK);
    char v[8];
    assert(tbox_db_meta_get(again, TBOX_DB_META_DB_KEY_SET, v, sizeof v) == 1);
    assert(strcmp(v, "1") == 0);
    /* set again = replace, not a primary-key error */
    assert(tbox_db_meta_set(again, TBOX_DB_META_DB_KEY_SET, "0") == TBOX_DB_OK);
    assert(tbox_db_meta_get(again, TBOX_DB_META_DB_KEY_SET, v, sizeof v) == 1);
    assert(strcmp(v, "0") == 0);
    tbox_db_close(again);
    printf("store: reopen keeps meta and does not re-migrate\n");
}

static void test_refuses_a_newer_schema(void) {
    char path[256];
    snprintf(path, sizeof path, "%s/index.db", DIR);
    sqlite3 *h = raw_open(path);
    char sql[64];
    /* pretend a future build wrote version 999 */
    snprintf(sql, sizeof sql,
             "UPDATE meta SET value='999' WHERE key='schema_version'");
    char *err = NULL;
    assert(sqlite3_exec(h, sql, NULL, NULL, &err) == SQLITE_OK);
    sqlite3_free(err);
    sqlite3_close(h);

    TBoxDb *db = NULL;
    assert(tbox_db_open(DIR, &db) == TBOX_DB_E_TOO_NEW);
    assert(db == NULL);

    /* put it back so a rerun of this test starts clean */
    h = raw_open(path);
    sqlite3_exec(h, "UPDATE meta SET value='2' WHERE key='schema_version'",
                 NULL, NULL, NULL);
    sqlite3_close(h);
    printf("store: a newer on-disk schema is refused\n");
}

static void test_bad_arguments(void) {
    TBoxDb *db = NULL;
    assert(tbox_db_open(NULL, &db) == TBOX_DB_E_ARGS);
    assert(tbox_db_open(DIR, NULL) == TBOX_DB_E_ARGS);
    assert(tbox_db_open("", &db) == TBOX_DB_E_ARGS);
    /* NULL-safe close */
    tbox_db_close(NULL);
    printf("store: bad arguments refused, NULL close is a no-op\n");
}

/* A file written by the previous build (v1: meta only) must upgrade in place:
 * the meta rows survive and the catalog tables appear. This is the promise
 * that makes the numbered migrations worth having. */
static void test_upgrade_from_v1(void) {
    const char *dir = "tbox_store_test_v1";
    (void)tbox_fsx_remove("tbox_store_test_v1/index.db");
    (void)tbox_fsx_remove("tbox_store_test_v1/index.db-wal");
    (void)tbox_fsx_remove("tbox_store_test_v1/index.db-shm");
    assert(tbox_fsx_mkdirs(dir) == TBOX_FSX_OK);

    /* hand-build exactly what v1 produced, with a meta row to preserve */
    char path[256];
    snprintf(path, sizeof path, "%s/index.db", dir);
    sqlite3 *h = raw_open(path);
    char *err = NULL;
    assert(sqlite3_exec(h,
                        "CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);"
                        "INSERT INTO meta(key,value) VALUES('schema_version','1');"
                        "INSERT INTO meta(key,value) VALUES('db_key_set','1');",
                        NULL, NULL, &err) == SQLITE_OK);
    sqlite3_free(err);
    sqlite3_close(h);

    TBoxDb *db = NULL;
    assert(tbox_db_open(dir, &db) == TBOX_DB_OK);
    assert(tbox_db_schema_version(db) == TBOX_DB_SCHEMA_VERSION);
    char v[32];
    /* the old row survived the upgrade */
    assert(tbox_db_meta_get(db, TBOX_DB_META_DB_KEY_SET, v, sizeof v) == 1);
    assert(strcmp(v, "1") == 0);
    tbox_db_close(db);

    /* and the catalog tables are there now */
    h = raw_open(path);
    assert(table_exists(h, "messages"));
    assert(table_exists(h, "objects"));
    assert(table_exists(h, "ops"));
    sqlite3_close(h);
    printf("store: a v1 file upgrades to v%d keeping its meta\n",
           TBOX_DB_SCHEMA_VERSION);
}

int main(void) {
    (void)tbox_fsx_remove("tbox_store_test/index.db");
    (void)tbox_fsx_remove("tbox_store_test/index.db-wal");
    (void)tbox_fsx_remove("tbox_store_test/index.db-shm");
    assert(tbox_fsx_mkdirs(DIR) == TBOX_FSX_OK);

    test_bad_arguments();
    test_migration_from_empty();
    test_reopen_is_idempotent();
    test_upgrade_from_v1();
    test_refuses_a_newer_schema();
    printf("store OK\n");
    return 0;
}
