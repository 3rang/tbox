/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * db.c — open index.db, run numbered migrations, read/write meta (spec 7.5).
 *
 * Migrations are an ordered list of SQL scripts. Each runs inside one
 * transaction together with its schema_version bump, so a crash halfway
 * leaves the file at the previous version, never half-migrated. Nothing in
 * here ever builds SQL from user text: meta keys/values go through bound
 * parameters, and every statement is prepared once and finalized.
 *
 * Path note (spec 16): sqlite3_open takes a UTF-8 filename on every
 * platform; the bundled win32 VFS converts it to UTF-16 and calls the W
 * APIs internally, so a profile under a non-ASCII user name works with no
 * conversion here.
 */
#include "store/db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite3.h"

struct TBoxDb {
    sqlite3 *handle;
    int schema_version;
};

/* ---- migration list -------------------------------------------------- */

/* One migration: the version it produces and the script that gets there.
 * Append only; never edit a shipped entry. */
typedef struct TBoxDbMigration {
    int version;
    const char *sql;
} TBoxDbMigration;

/* v1: the meta table only. The catalog tables (messages, objects, ops) land
 * in their own migration; splitting them keeps the spec's promise that
 * index.db is built up in numbered steps rather than one grand script. */
static const TBoxDbMigration MIGRATIONS[] = {
    { 1,
      "CREATE TABLE meta(\n"
      "  key   TEXT PRIMARY KEY,\n"
      "  value TEXT NOT NULL\n"
      ");\n" },
};

static const size_t MIGRATION_COUNT = sizeof(MIGRATIONS) / sizeof(MIGRATIONS[0]);

/* ---- small helpers --------------------------------------------------- */

/* Joins a profile dir and a leaf file name with one separator. */
static TBoxDbStatus join_path(char *out, size_t out_size, const char *dir,
                              const char *leaf) {
    size_t n = strlen(dir);
    int need_sep = (n > 0 && dir[n - 1] != '/' && dir[n - 1] != '\\');
    int wrote = snprintf(out, out_size, "%s%s%s", dir, need_sep ? "\\" : "", leaf);
    if (wrote < 0 || (size_t)wrote >= out_size) {
        return TBOX_DB_E_ARGS;
    }
    return TBOX_DB_OK;
}

/* Runs one statement with no result rows. */
static TBoxDbStatus exec_sql(sqlite3 *handle, const char *sql,
                             const char *what) {
    char *err = NULL;
    int rc = sqlite3_exec(handle, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        /* err is SQLite's own text: no user data, safe to drop silently */
        sqlite3_free(err);
        (void)what;
        return TBOX_DB_E_IO;
    }
    return TBOX_DB_OK;
}

/* Reads an integer out of `SELECT value FROM meta WHERE key='schema_version'`.
 * A missing row means a brand-new file (version 0). */
static int read_schema_version(sqlite3 *handle) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(handle,
                           "SELECT value FROM meta WHERE key=?1", -1, &st,
                           NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, TBOX_DB_META_SCHEMA_VERSION, -1, SQLITE_STATIC);
    int version = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *v = sqlite3_column_text(st, 0);
        version = (v != NULL) ? atoi((const char *)v) : 0;
    }
    sqlite3_finalize(st);
    return version;
}

/* Writes the schema_version row (insert or replace). */
static TBoxDbStatus write_schema_version(sqlite3 *handle, int version) {
    char buf[16];
    snprintf(buf, sizeof buf, "%d", version);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(handle,
                           "INSERT INTO meta(key,value) VALUES(?1,?2)\n"
                           "  ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                           -1, &st, NULL) != SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    sqlite3_bind_text(st, 1, TBOX_DB_META_SCHEMA_VERSION, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, buf, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? TBOX_DB_OK : TBOX_DB_E_IO;
}

/* ---- migrations ------------------------------------------------------ */

/* Brings the on-disk schema up to TBOX_DB_SCHEMA_VERSION. Each pending
 * migration runs in its own transaction (BEGIN IMMEDIATE ... COMMIT) with
 * its version bump, so a failure rolls the whole step back. */
static TBoxDbStatus migrate(sqlite3 *handle, int from) {
    if (from > TBOX_DB_SCHEMA_VERSION) {
        return TBOX_DB_E_TOO_NEW;
    }
    for (size_t i = 0; i < MIGRATION_COUNT; i++) {
        const TBoxDbMigration *m = &MIGRATIONS[i];
        if (m->version <= from) {
            continue;
        }
        if (exec_sql(handle, "BEGIN IMMEDIATE", "begin") != TBOX_DB_OK) {
            return TBOX_DB_E_MIGRATE;
        }
        if (exec_sql(handle, m->sql, "migration") != TBOX_DB_OK ||
            write_schema_version(handle, m->version) != TBOX_DB_OK) {
            exec_sql(handle, "ROLLBACK", "rollback");
            return TBOX_DB_E_MIGRATE;
        }
        if (exec_sql(handle, "COMMIT", "commit") != TBOX_DB_OK) {
            exec_sql(handle, "ROLLBACK", "rollback");
            return TBOX_DB_E_MIGRATE;
        }
    }
    return TBOX_DB_OK;
}

/* ---- open / close ---------------------------------------------------- */

TBoxDbStatus tbox_db_open(const char *profile_dir, TBoxDb **out) {
    if (out == NULL || profile_dir == NULL || profile_dir[0] == '\0') {
        return TBOX_DB_E_ARGS;
    }
    *out = NULL;

    char path[700];
    TBoxDbStatus js = join_path(path, sizeof path, profile_dir, "index.db");
    if (js != TBOX_DB_OK) {
        return js;
    }

    TBoxDb *db = calloc(1, sizeof *db);
    if (db == NULL) {
        return TBOX_DB_E_MEM;
    }
    if (sqlite3_open(path, &db->handle) != SQLITE_OK) {
        /* handle may exist even on failure; freeing closes it */
        sqlite3_close(db->handle);
        free(db);
        return TBOX_DB_E_OPEN;
    }

    /* spec 7.5: WAL, foreign_keys=ON, synchronous=NORMAL. These are
     * connection settings; failure is not fatal to opening. */
    (void)exec_sql(db->handle, "PRAGMA journal_mode=WAL", "wal");
    (void)exec_sql(db->handle, "PRAGMA foreign_keys=ON", "fk");
    (void)exec_sql(db->handle, "PRAGMA synchronous=NORMAL", "sync");
    /* wait rather than fail if another connection holds a write lock */
    (void)exec_sql(db->handle, "PRAGMA busy_timeout=5000", "busy");

    int from = read_schema_version(db->handle);
    if (from < 0) {
        /* no meta table yet (fresh file) -> version 0 */
        from = 0;
    }
    TBoxDbStatus ms = migrate(db->handle, from);
    if (ms != TBOX_DB_OK) {
        sqlite3_close(db->handle);
        free(db);
        return ms;
    }
    db->schema_version = (from > TBOX_DB_SCHEMA_VERSION) ? from
                                                         : TBOX_DB_SCHEMA_VERSION;
    *out = db;
    return TBOX_DB_OK;
}

void tbox_db_close(TBoxDb *db) {
    if (db == NULL) {
        return;
    }
    sqlite3_close(db->handle);
    free(db);
}

int tbox_db_schema_version(const TBoxDb *db) {
    return (db != NULL) ? db->schema_version : 0;
}

/* ---- meta ------------------------------------------------------------ */

int tbox_db_meta_get(TBoxDb *db, const char *key, char *out, size_t out_size) {
    if (db == NULL || key == NULL || out == NULL || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle, "SELECT value FROM meta WHERE key=?1",
                           -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    int found = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *v = sqlite3_column_text(st, 0);
        if (v != NULL) {
            snprintf(out, out_size, "%s", (const char *)v);
        }
        found = 1;
    }
    sqlite3_finalize(st);
    return found;
}

int tbox_db_pragma_int(const TBoxDb *db, const char *name) {
    if (db == NULL || name == NULL || name[0] == '\0') {
        return -1;
    }
    /* name is a fixed literal from our own code, never user text; a pragma
     * cannot take a bound parameter, so it is spliced - guarded by an
     * allow-list of the pragmas the engine actually sets. */
    static const char *const allowed[] = { "synchronous", "foreign_keys",
                                           "journal_mode", "busy_timeout" };
    int ok = 0;
    for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; i++) {
        if (strcmp(name, allowed[i]) == 0) {
            ok = 1;
            break;
        }
    }
    if (!ok) {
        return -1;
    }
    char sql[64];
    snprintf(sql, sizeof sql, "PRAGMA %s", name);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle, sql, -1, &st, NULL) != SQLITE_OK) {
        return -1;
    }
    int value = -1;
    if (sqlite3_step(st) == SQLITE_ROW) {
        value = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return value;
}

TBoxDbStatus tbox_db_meta_set(TBoxDb *db, const char *key, const char *value) {
    if (db == NULL || key == NULL || value == NULL) {
        return TBOX_DB_E_ARGS;
    }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->handle,
                           "INSERT INTO meta(key,value) VALUES(?1,?2)\n"
                           "  ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                           -1, &st, NULL) != SQLITE_OK) {
        return TBOX_DB_E_IO;
    }
    sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, value, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE) ? TBOX_DB_OK : TBOX_DB_E_IO;
}
