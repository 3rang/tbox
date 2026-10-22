/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * items.c — the spec 7.6 item shape, as JSON (see items.h).
 *
 * Absent values are null, not zero and not an empty string: the UI has to be
 * able to tell "no size known" from "size 0".
 */
#include "engine/items.h"

#include <stdlib.h>
#include <string.h>

/* Adds a string field, or null when the value is unknown. */
static void add_str(cJSON *obj, const char *name, const char *value) {
    if (value == NULL || value[0] == '\0') {
        cJSON_AddNullToObject(obj, name);
    } else {
        cJSON_AddStringToObject(obj, name, value);
    }
}

/* Adds a number field, or null when the value is unknown. */
static void add_num(cJSON *obj, const char *name, int has, long long value) {
    if (!has) {
        cJSON_AddNullToObject(obj, name);
    } else {
        cJSON_AddNumberToObject(obj, name, (double)value);
    }
}

cJSON *tbox_item_json(const TBoxListItem *item) {
    cJSON *o = cJSON_CreateObject();
    if (o == NULL || item == NULL) {
        cJSON_Delete(o);
        return NULL;
    }
    add_str(o, "object_id", item->object_id);
    cJSON_AddStringToObject(o, "kind", item->kind == 'd' ? "d" : "f");
    add_str(o, "name", item->name);
    add_str(o, "parent_id", item->parent_id);
    cJSON_AddNumberToObject(o, "rev", (double)item->rev);
    add_num(o, "size", item->has_size, item->size);
    add_num(o, "mtime", item->has_mtime, item->mtime);
    add_num(o, "trashed_at", item->has_trashed, item->trashed_at);
    cJSON_AddBoolToObject(o, "conflict", item->conflict != 0);
    add_str(o, "message_id", item->message_id);
    add_str(o, "mime", item->mime);
    return o;
}

cJSON *tbox_items_json(const TBoxListItem *items, size_t count) {
    cJSON *arr = cJSON_CreateArray();
    if (arr == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        cJSON *o = tbox_item_json(&items[i]);
        if (o == NULL) {
            cJSON_Delete(arr);
            return NULL;
        }
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

cJSON *tbox_inbox_json(const TBoxInboxRow *items, size_t count) {
    cJSON *arr = cJSON_CreateArray();
    if (arr == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        const TBoxInboxRow *r = &items[i];
        cJSON *o = cJSON_CreateObject();
        if (o == NULL) {
            cJSON_Delete(arr);
            return NULL;
        }
        cJSON_AddNullToObject(o, "object_id");
        add_str(o, "message_id", r->message_id);
        add_str(o, "orig_name", r->orig_name);
        add_num(o, "size", r->has_size, r->size);
        add_str(o, "mime", r->mime);
        cJSON_AddNumberToObject(o, "date", (double)r->date);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

cJSON *tbox_revisions_json(const TBoxRevision *revs, size_t count) {
    cJSON *arr = cJSON_CreateArray();
    if (arr == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        const TBoxRevision *r = &revs[i];
        cJSON *o = cJSON_CreateObject();
        if (o == NULL) {
            cJSON_Delete(arr);
            return NULL;
        }
        add_str(o, "message_id", r->message_id);
        cJSON_AddNumberToObject(o, "rev", (double)r->rev);
        cJSON_AddNumberToObject(o, "date", (double)r->date);
        add_num(o, "size", r->has_size, r->size);
        add_str(o, "sha256", r->has_sha256 ? r->sha256 : NULL);
        add_num(o, "mtime", r->has_mtime, r->mtime);
        add_num(o, "trashed_at", r->has_trashed, r->trashed_at);
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

/* One (files, folders, bytes) group. */
static void add_group(cJSON *parent, const char *name, long long files,
                      long long folders, long long bytes) {
    cJSON *g = cJSON_AddObjectToObject(parent, name);
    if (g == NULL) {
        return;
    }
    cJSON_AddNumberToObject(g, "files", (double)files);
    cJSON_AddNumberToObject(g, "folders", (double)folders);
    cJSON_AddNumberToObject(g, "bytes", (double)bytes);
}

cJSON *tbox_summary_json(const TBoxCatalogSummary *s) {
    if (s == NULL) {
        return NULL;
    }
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return NULL;
    }
    add_group(o, "objects", s->files, s->folders, s->bytes);
    add_group(o, "root", s->root_files, s->root_folders, s->root_bytes);
    add_group(o, "lost", s->lost_files, s->lost_folders, s->lost_bytes);
    add_group(o, "trash", s->trash_files, s->trash_folders, s->trash_bytes);
    cJSON *inbox = cJSON_AddObjectToObject(o, "inbox");
    if (inbox != NULL) {
        cJSON_AddNumberToObject(inbox, "items", (double)s->inbox_items);
        cJSON_AddNumberToObject(inbox, "bytes", (double)s->inbox_bytes);
    }
    cJSON *top = cJSON_AddArrayToObject(o, "top_folders");
    if (top != NULL) {
        for (size_t i = 0; i < s->top_count; i++) {
            const TBoxFolderTotal *t = &s->top[i];
            cJSON *f = cJSON_CreateObject();
            if (f == NULL) {
                break;
            }
            add_str(f, "folder_id", t->folder_id);
            add_str(f, "name", t->name);
            cJSON_AddNumberToObject(f, "files", (double)t->files);
            cJSON_AddNumberToObject(f, "folders", (double)t->folders);
            cJSON_AddNumberToObject(f, "bytes", (double)t->bytes);
            cJSON_AddItemToArray(top, f);
        }
    }
    return o;
}
