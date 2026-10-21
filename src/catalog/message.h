/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * message.h — read one TDLib message into the shape of a `messages` row
 * (spec 7.1, 7.2, 7.5). Pure: JSON in, a plain struct out. No TDLib symbols,
 * no SQLite, no filesystem, so the scan and the update path share it and its
 * tests need nothing but cJSON.
 *
 * The classification it performs is exactly spec 7.1:
 *   - a document/photo/video/audio/voice whose caption is a valid record with
 *     k="f" is a managed file;
 *   - a text message whose text is a valid record with k="d" is a managed
 *     folder;
 *   - a document/photo/... without a valid record is an unmanaged file;
 *   - a text message without a valid record is a note;
 *   - anything else is `other` and the catalog ignores it.
 * A caption or text that starts with the managed prefix but does not parse
 * keeps its `bad_record` flag so doctor.run can report it.
 */
#ifndef TBOX_CATALOG_MESSAGE_H
#define TBOX_CATALOG_MESSAGE_H

#include "catalog/record.h"

struct cJSON; /* forward declaration: callers pass a parsed TDLib message */

/* The `kind` values stored in `messages.kind` (spec 7.5). */
#define TBOX_MSG_KIND_FILE      "file"
#define TBOX_MSG_KIND_FOLDER    "folder"
#define TBOX_MSG_KIND_UNMANAGED "unmanaged"
#define TBOX_MSG_KIND_NOTE      "note"

/* One row of the `messages` table. Strings are fixed-size copies, so the
 * struct is trivially copyable and owns no heap. Absent optional fields are
 * flagged rather than inferred from an empty string. */
typedef struct TBoxMessageRow {
    char        message_id[TBOX_MSGID_MAX_BYTES + 1];
    char        kind[16];      /* file/folder/unmanaged/note */
    long long   date;
    int         has_file_id;
    long long   file_id;
    int         has_remote_id;
    long long   remote_id;
    char        mime[96];
    int         has_size;
    long long   size;
    char        orig_name[TBOX_NAME_MAX_BYTES * 4 + 1]; /* Telegram's own name */
    char       *text;          /* heap: caption or note text (may be NULL) */
    int         has_record;    /* a valid record was parsed */
    TBoxRecord  rec;           /* valid only when has_record */
    int         bad_record;    /* managed prefix seen, body did not parse */
} TBoxMessageRow;

/* Parses a TDLib `message` object into out. Returns 1 when the message is one
 * the catalog cares about (its kind was filled in), 0 when it is `other` and
 * should be ignored, and -1 on a malformed message (no id, not an object).
 *
 * out->text is heap-allocated when the message carries text; free it with
 * tbox_message_row_free(). */
int tbox_message_parse(const struct cJSON *message, TBoxMessageRow *out);

/* Frees the heap field of a row. Safe on a zeroed row and safe twice. */
void tbox_message_row_free(TBoxMessageRow *row);

#endif /* TBOX_CATALOG_MESSAGE_H */
