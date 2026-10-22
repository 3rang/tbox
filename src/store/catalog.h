/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * catalog.h — the SQLite side of the catalog: writing `messages`, deleting
 * rows a scan did not see, and swapping in the resolved `objects` (spec 7.5,
 * 8.2). The connection still belongs to store/db.c; this module only prepares
 * statements against it.
 *
 * Every value is bound, never spliced: nothing here builds SQL from a name,
 * an id or any other string that came from Telegram or the UI.
 */
#ifndef TBOX_STORE_CATALOG_H
#define TBOX_STORE_CATALOG_H

#include "catalog/message.h"
#include "catalog/resolve.h"
#include "store/db.h"

/* ---- transactions ---------------------------------------------------- */

/* A scan writes page by page, each page in its own transaction (8.2), so a
 * crash midway keeps the pages already written but never a half page. */
TBoxDbStatus tbox_cat_begin(TBoxDb *db);
TBoxDbStatus tbox_cat_commit(TBoxDb *db);
TBoxDbStatus tbox_cat_rollback(TBoxDb *db);

/* ---- messages -------------------------------------------------------- */

/* Inserts or replaces one `messages` row. */
TBoxDbStatus tbox_cat_upsert_message(TBoxDb *db, const TBoxMessageRow *row);

/* 1 when a `messages` row with this id exists, else 0. */
int tbox_cat_has_message(TBoxDb *db, const char *message_id);

/* Deletes the messages with these ids (the rows a scan did not see). */
TBoxDbStatus tbox_cat_delete_messages(TBoxDb *db, const char *const *ids,
                                      size_t count);

/* Points any op that was waiting on `old_id` at `new_id`, the temp-to-real
 * message id mapping of spec 8.3. A no-op when no op refers to the old id (the
 * normal case before Phase 2 fills `ops`). */
TBoxDbStatus tbox_cat_remap_temp_message(TBoxDb *db, const char *old_id,
                                         const char *new_id);

/* Collects every message_id currently in `messages` into a heap array of heap
 * strings (the scan subtracts the ids it saw to find the stale ones). Free the
 * result with tbox_cat_free_ids(). Returns TBOX_DB_OK on success. */
TBoxDbStatus tbox_cat_list_message_ids(TBoxDb *db, char ***ids, size_t *count);

/* Frees an id list from tbox_cat_list_message_ids and zeroes it. */
void tbox_cat_free_ids(char **ids, size_t count);

/* Loads every `messages` row into a heap array of rows, for re-resolving after
 * an incremental update (spec 8.3). The array is sized once, so a caller may
 * hold pointers into it while it builds TBoxResolveMsg views. Free with
 * tbox_cat_free_messages(). */
TBoxDbStatus tbox_cat_load_messages(TBoxDb *db, TBoxMessageRow **rows,
                                    size_t *count);

/* Frees a load from tbox_cat_load_messages (including each row's text). */
void tbox_cat_free_messages(TBoxMessageRow *rows, size_t count);

/* ---- objects --------------------------------------------------------- */

/* Rebuilds `objects` from the `messages` table (spec 7.4: objects is derived
 * data, so the resolver is the only thing that may write it). The rows are
 * loaded and re-resolved here, which is what makes an incremental change and
 * a fresh scan land on the same catalog. objects/inbox may be NULL. */
TBoxDbStatus tbox_cat_rebuild_objects(TBoxDb *db, long long *objects,
                                      long long *inbox);

/* Replaces the whole `objects` table with the resolver's output: it deletes
 * every row, then inserts the new ones. `objects` is derived data (7.5), so a
 * wholesale swap is exactly right and keeps resolve the single source of
 * truth. Runs inside its own transaction. */
TBoxDbStatus tbox_cat_replace_objects(TBoxDb *db, const TBoxResolveResult *res);

#endif /* TBOX_STORE_CATALOG_H */
