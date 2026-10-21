/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * resolve.h — the core invariant of spec 7.4: turn the set of messages in
 * Saved Messages into the resolved catalog (objects, the Inbox, and the
 * findings doctor.run reports).
 *
 * This module is pure. It reads an array of messages - exactly what the
 * `messages` table holds, plus the parsed record - and writes objects, Inbox
 * items and diagnostics into caller-provided arrays. No SQLite, no TDLib, no
 * filesystem. That is what makes "full scan == incremental" a property the
 * tests can check directly, because both paths call this same function.
 *
 * The rules, verbatim from 7.4:
 *   - per object id, the current revision is the highest `r`;
 *   - a tie on the highest `r` is a conflict: the higher message id wins, the
 *     others become separate conflict items named `<name> (conflict <id>)`;
 *   - a `k` that differs between revisions is a kind mismatch (keep the
 *     higher `r`, report it);
 *   - a parent that is missing or not a live folder sends the item to `lost`;
 *   - a parent cycle sends every item in the cycle to `lost`;
 *   - trashed items live only in `trash`, and hide their children;
 *   - two live items of the same name in one folder are both kept (reported);
 *   - the Inbox is every unmanaged file whose message id is not some record's
 *     `src`.
 */
#ifndef TBOX_CATALOG_RESOLVE_H
#define TBOX_CATALOG_RESOLVE_H

#include <stddef.h>

#include "catalog/record.h"

/* The virtual folder ids (spec 7.4). A real folder id is 32 lowercase hex. */
#define TBOX_FOLDER_ROOT  "root"
#define TBOX_FOLDER_LOST  "lost"
#define TBOX_FOLDER_TRASH "trash"
#define TBOX_FOLDER_INBOX "inbox"

/* A resolved object id is either the record's 32 hex, or - for a conflict
 * item - that id, a dash, and the losing message id (spec 7.4 rule 3, keyed
 * this way so the extra revisions stay addressable under the objects table's
 * single-column primary key). */
#define TBOX_RESOLVE_ID_MAX (TBOX_ID_HEX_LEN + 1 + TBOX_MSGID_MAX_BYTES)

typedef enum TBoxMsgKind {
    TBOX_MSG_FILE = 0,   /* managed file (valid record, k=f) */
    TBOX_MSG_FOLDER,     /* managed folder (valid record, k=d) */
    TBOX_MSG_UNMANAGED,  /* a file/photo/video/audio without a valid record */
    TBOX_MSG_NOTE,       /* a text message without a valid record */
    TBOX_MSG_OTHER       /* anything else: ignored by the catalog */
} TBoxMsgKind;

/* One message as the resolver sees it. This is a narrowed view of the
 * `messages` table row; the caller fills it from SQLite (or, in tests, by
 * hand). `rec` is meaningful only when `kind` is FILE or FOLDER. */
typedef struct TBoxResolveMsg {
    const char *message_id;  /* decimal string, used for ordering and naming */
    TBoxMsgKind kind;
    long long   date;        /* Unix seconds */
    const char *orig_name;   /* file name from Telegram, or NULL */
    const char *mime;        /* MIME type, or NULL */
    int         has_size;    /* size present (unmanaged files) */
    long long   size;
    TBoxRecord  rec;         /* valid only for FILE/FOLDER */
    int         bad_record;  /* managed prefix but invalid body */
} TBoxResolveMsg;

/* One resolved object (a row of the `objects` table, spec 7.5). Strings point
 * into the input messages or into `storage`, never into freed memory: the
 * whole result is valid until tbox_resolve_result_free(). */
typedef struct TBoxObject {
    char        object_id[TBOX_RESOLVE_ID_MAX + 1];
    char        kind;                    /* 'f' or 'd' */
    const char *parent_id;               /* object id, or root/lost/trash */
    const char *name;                    /* may be a synthesized conflict name */
    long long   rev;
    const char *message_id;              /* the current revision's message */
    int         has_size;
    long long   size;
    const char *sha256;                  /* or NULL */
    int         has_mtime;
    long long   mtime;
    int         has_trashed;
    long long   trashed_at;
    int         conflict;                /* an extra (non-current) revision */
} TBoxObject;

/* One Inbox item (spec 7.6): an unmanaged file not adopted anywhere. */
typedef struct TBoxInboxItem {
    const char *message_id;
    const char *orig_name;  /* or NULL */
    const char *mime;       /* or NULL */
    int         has_size;
    long long   size;
    long long   date;
} TBoxInboxItem;

/* A finding doctor.run reports (spec 8.8). Only the codes resolve.c can know
 * about are produced here. */
typedef enum TBoxFindingCode {
    TBOX_FIND_BAD_RECORD = 0,
    TBOX_FIND_CONFLICT,
    TBOX_FIND_DUPLICATE_NAME,
    TBOX_FIND_LOST_PARENT,
    TBOX_FIND_CYCLE,
    TBOX_FIND_KIND_MISMATCH
} TBoxFindingCode;

typedef struct TBoxFinding {
    TBoxFindingCode code;
    char        object_id[TBOX_RESOLVE_ID_MAX + 1]; /* "" when not object-specific */
    const char *message_id;                     /* or NULL */
    const char *detail;                         /* short, human-readable or NULL */
} TBoxFinding;

typedef struct TBoxResolveResult {
    TBoxObject    *objects;
    size_t         object_count;
    TBoxInboxItem *inbox;
    size_t         inbox_count;
    TBoxFinding   *findings;
    size_t         finding_count;

    /* Interned strings for synthesized names (conflict labels) and details,
     * so no object points at a temporary buffer. Freed by ..._free. */
    char         **arena;
    size_t         arena_count;
} TBoxResolveResult;

/* Resolves msgs[0..count) into out. Arrays are grown as needed (the result
 * owns them; free with tbox_resolve_result_free). `out` must be zeroed by the
 * caller - it is, by tbox_resolve. Returns 0 on success, -1 on out-of-memory
 * (out is left freeable). */
int tbox_resolve(const TBoxResolveMsg *msgs, size_t count,
                 TBoxResolveResult *out);

/* Frees every array the result owns and zeroes it. Safe on a zeroed struct
 * and safe to call twice. */
void tbox_resolve_result_free(TBoxResolveResult *out);

/* The findings code as the spec's uppercase token (for doctor.run). */
const char *tbox_finding_code_name(TBoxFindingCode code);

#endif /* TBOX_CATALOG_RESOLVE_H */
