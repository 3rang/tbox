/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * scanmap.c - one Saved Messages document -> one archive entry.
 *
 * The caption, when it is ours, is authoritative: it carries the logical
 * path and, for files, the sha256 we recorded at upload time. Everything
 * else - a document we uploaded from a phone, a forwarded photo - is named
 * after the document itself.
 *
 * v0.3 semantics, kept deliberately:
 *   - only documents are visible; a text-only message is not an entry
 *   - a caption without a usable name falls back to the document's name
 *   - two documents that resolve to the same name are one entry, newest wins
 */

#include "core/scanmap.h"

#include <stdio.h>
#include <string.h>

/* The last resort: a name we know is unique because it carries the message id. */
static void use_message_id(long long message_id, char *out, size_t size)
{
    int n = snprintf(out, size, "message-%lld", message_id);

    /* a positive id always fits; the guard is for out-of-range ids */
    if (n < 0 || (size_t)n >= size)
        snprintf(out, size, "message");
}

int tbox_scanmap_entry(long long message_id, const char *text,
                       const char *file_name, long long size,
                       long long date, long long remote_id,
                       tbox_archive_entry_t *out)
{
    tbox_archive_entry_t e;

    if (out == NULL)
        return -1;

    /* Our caption: it owns the name and the kind. caption_parse() has already
     * normalized the name and trimmed trailing separators, so a successful
     * parse plus a non-empty name is enough to trust it. */
    memset(&e, 0, sizeof e);
    if (tbox_caption_parse(text, &e) == 0 && e.name[0] != '\0') {
        /* caption mtime is -1 when absent (v0.3 marker). */
        if (e.mtime < 0)
            e.mtime = date > 0 ? date : -1;
        if (!e.is_dir && e.size <= 0)
            e.size = size > 0 ? size : 0;
        e.message_id = message_id;
        e.file_id = remote_id;

        *out = e;
        return 0;
    }

    /* A foreign document: no caption we understand, so it is named after the
     * document and carries no sha256 (is_tbox stays 0, which is what stops a
     * later rename/delete from pretending we own it). */
    memset(&e, 0, sizeof e);
    e.message_id = message_id;
    e.file_id = remote_id;
    e.is_dir = 0;
    e.size = size > 0 ? size : 0;
    e.mtime = date > 0 ? date : -1;

    if (file_name != NULL && file_name[0] != '\0'
        && tbox_archive_name(file_name, e.name, sizeof e.name) == 0
        && e.name[0] != '\0') {
        *out = e;
        return 0;
    }

    use_message_id(message_id, e.name, sizeof e.name);
    *out = e;

    return 0;
}
