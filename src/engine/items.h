/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * items.h — the spec 7.6 item shape, as JSON.
 *
 * The store hands over plain structs; this turns them into the objects the
 * protocol returns. It is pure (no SQLite, no engine state), so the shape of
 * a catalog item can be tested without an engine.
 */
#ifndef TBOX_ENGINE_ITEMS_H
#define TBOX_ENGINE_ITEMS_H

#include <stddef.h>

#include "cJSON.h"
#include "store/query.h"

/* One item (spec 7.6). Never NULL unless memory ran out. */
cJSON *tbox_item_json(const TBoxListItem *item);

/* A list of items as a JSON array. */
cJSON *tbox_items_json(const TBoxListItem *items, size_t count);

/* The Inbox (spec 7.6: object_id is null). */
cJSON *tbox_inbox_json(const TBoxInboxRow *items, size_t count);

/* The revisions of one object, newest first. */
cJSON *tbox_revisions_json(const TBoxRevision *revs, size_t count);

/* catalog.summary. */
cJSON *tbox_summary_json(const TBoxCatalogSummary *summary);

#endif /* TBOX_ENGINE_ITEMS_H */
