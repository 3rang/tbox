/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * db.h — the index.db catalog: one SQLite connection, numbered migrations
 * (spec 7.5).
 *
 * index.db is derived data that the engine alone owns (spec 3.1 rule 5: the
 * UI never opens it). This layer knows only how to open the file, bring its
 * schema up to date in a transaction, and read/write the small `meta` table
 * (schema_version, account_user_id, last_full_scan_at, db_key_set, ...).
 * The catalog tables (messages, objects, ops; spec 7.5) are created here too;
 * the code that fills and reads them lives in the catalog and sync modules.
 *
 * Concurrency: one connection, owned by the engine's state thread. The
 * library is compiled threadsafe, but no two threads share a connection
 * here; the engine loop is the only writer (spec 3.1 rule 2).
 */
#ifndef TBOX_STORE_DB_H
#define TBOX_STORE_DB_H

#include <stddef.h>

typedef enum TBoxDbStatus {
    TBOX_DB_OK = 0,
    TBOX_DB_E_ARGS = -1,    /* NULL/empty argument */
    TBOX_DB_E_OPEN = -2,    /* the file could not be created/opened */
    TBOX_DB_E_MIGRATE = -3, /* a migration failed; nothing was committed */
    TBOX_DB_E_IO = -4,      /* a statement failed at runtime */
    TBOX_DB_E_MEM = -5,
    TBOX_DB_E_TOO_NEW = -6  /* on-disk schema is newer than this build */
} TBoxDbStatus;

typedef struct TBoxDb TBoxDb;

/* The schema version this build writes. Bump it and append a migration when
 * a table changes; never edit an existing migration. v1 is the meta table,
 * v2 adds the catalog tables (messages, objects, ops). */
#define TBOX_DB_SCHEMA_VERSION 2

/* meta keys used by the engine. */
#define TBOX_DB_META_SCHEMA_VERSION   "schema_version"
#define TBOX_DB_META_DB_KEY_SET       "db_key_set"
#define TBOX_DB_META_ACCOUNT_USER_ID  "account_user_id"
#define TBOX_DB_META_LAST_FULL_SCAN   "last_full_scan_at"
#define TBOX_DB_META_SCAN_COMPLETE    "scan_complete"
#define TBOX_DB_META_CACHE_LIMIT      "cache_limit_bytes"

/* Opens <profile_dir>/index.db, creating it if missing, sets WAL +
 * foreign_keys + synchronous=NORMAL (spec 7.5) and runs pending migrations
 * in one transaction. Returns TBOX_DB_E_TOO_NEW (and leaves the file
 * untouched) when the file was written by a newer build. */
TBoxDbStatus tbox_db_open(const char *profile_dir, TBoxDb **out);

/* Closes the file. Sets *db NULL. Safe with *db == NULL. */
void tbox_db_close(TBoxDb *db);

/* The schema version currently on disk. */
int tbox_db_schema_version(const TBoxDb *db);

/* Reads a meta value into out (NUL-terminated). Returns 1 when the key
 * exists, 0 when it does not, and -1 on a real error. */
int tbox_db_meta_get(TBoxDb *db, const char *key, char *out, size_t out_size);

/* Inserts or replaces a meta key. */
TBoxDbStatus tbox_db_meta_set(TBoxDb *db, const char *key, const char *value);

/* Reads an integer connection pragma (e.g. "synchronous", "foreign_keys")
 * on the engine's own connection. Returns the value, or -1 on error. */
int tbox_db_pragma_int(const TBoxDb *db, const char *name);

/* The underlying handle, for the store's own statement modules
 * (store/catalog.c). It is deliberately the only way out of this file: the
 * connection is still owned here and still single-threaded (spec 3.1 rule 2),
 * and callers must not open, close or re-configure it. Returns NULL for a
 * NULL db. */
struct sqlite3 *tbox_db_handle(TBoxDb *db);

#endif /* TBOX_STORE_DB_H */
