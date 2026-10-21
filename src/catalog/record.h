/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * record.h — the tbx1 record in a caption or text message (spec 7.2): parse
 * it, build it, and decide whether a name is allowed (S8).
 *
 * This module is pure. It touches no TDLib, no filesystem and no SQLite, so
 * it can be exercised by table-driven tests and reused from both the scan
 * path and the mutation path. Everything it produces is deterministic: the
 * same fields always serialize to the same bytes, in the fixed key order the
 * spec mandates.
 *
 * A record looks like:
 *
 *   tbx1 {"v":1,"k":"f","o":"<32 hex>","p":"root","n":"report.pdf","r":1,"s":12345}
 *
 * Parsing ignores keys it does not know (forward compatibility) and rejects
 * anything the spec says is invalid (unknown version, missing required key,
 * wrong type, bad id, bad name, too long).
 */
#ifndef TBOX_CATALOG_RECORD_H
#define TBOX_CATALOG_RECORD_H

#include <stddef.h>

/* The prefix every managed caption/text starts with, and the format version. */
#define TBOX_RECORD_PREFIX "tbx1 "
#define TBOX_RECORD_VERSION 1

/* Name limits and field sizes (spec 7.2, S8). */
#define TBOX_NAME_MAX_BYTES 255
#define TBOX_ID_HEX_LEN 32
#define TBOX_SHA256_HEX_LEN 64
#define TBOX_OP_HEX_LEN 16
/* A message id as text: Telegram ids are signed 32-bit, so a few digits plus
 * a sign; 32 bytes is generous and keeps the field fixed-size. */
#define TBOX_MSGID_MAX_BYTES 32

typedef enum TBoxRecordStatus {
    TBOX_REC_OK = 0,
    TBOX_REC_E_ARGS = -1,    /* NULL/empty argument, output buffer too small */
    TBOX_REC_E_NOPREFIX = -2,/* text does not start with "tbx1 " */
    TBOX_REC_E_SYNTAX = -3,  /* not JSON, or not a JSON object */
    TBOX_REC_E_VERSION = -4, /* v missing or not 1 */
    TBOX_REC_E_MISSING = -5, /* a required key is absent */
    TBOX_REC_E_TYPE = -6,    /* a key has the wrong JSON type */
    TBOX_REC_E_ID = -7,      /* o/p/h/q not the required hex shape */
    TBOX_REC_E_NAME = -8,    /* n fails the S8 rules */
    TBOX_REC_E_RANGE = -9,   /* r < 1, negative size, etc. */
    TBOX_REC_E_TOO_LONG = -10/* the whole record exceeds the caption limit */
} TBoxRecordStatus;

/* One parsed record. Strings are NUL-terminated copies bounded by the spec's
 * field sizes, so the struct is fixed-size and cheap to pass around. Optional
 * fields are flagged present/absent rather than guessed from empty strings -
 * a zero-length h is invalid, but absent h is normal. */
typedef struct TBoxRecord {
    int version;                 /* always TBOX_RECORD_VERSION when valid */
    char kind;                   /* 'f' or 'd' */
    char object_id[TBOX_ID_HEX_LEN + 1];
    char parent[TBOX_ID_HEX_LEN + 1]; /* 32 hex, or the literal "root" */
    char name[TBOX_NAME_MAX_BYTES + 1];
    long long rev;               /* r, >= 1 */
    int has_size;                /* s present */
    long long size;
    int has_sha256;
    char sha256[TBOX_SHA256_HEX_LEN + 1];
    int has_mtime;
    long long mtime;             /* m */
    int has_trashed;
    long long trashed_at;        /* t */
    int has_op;
    char op_id[TBOX_OP_HEX_LEN + 1]; /* q */
    int has_src;
    char src[TBOX_MSGID_MAX_BYTES + 1]; /* src (a message id, stored as text) */
} TBoxRecord;

/* Parses text (a caption or a text message) into out.
 *
 * Returns TBOX_REC_OK when text is a valid record; TBOX_REC_E_NOPREFIX when
 * it is not a managed record at all (the caller then treats the message as
 * unmanaged/note), and one of the other codes when it looks managed but is
 * invalid (bad_record). Never reads past the NUL; out is left untouched on
 * failure so a caller cannot act on half-parsed fields. */
TBoxRecordStatus tbox_record_parse(const char *text, TBoxRecord *out);

/* Serializes rec into out ("tbx1 {json}"), NUL-terminated. out_size must be
 * at least the record can need. Returns TBOX_REC_E_ARGS when out is too
 * small or rec is structurally invalid, and TBOX_REC_E_TOO_LONG when the
 * result would exceed max_len (the caption limit, spec 7.2). max_len of 0
 * means "no limit". */
TBoxRecordStatus tbox_record_build(const TBoxRecord *rec, char *out,
                                   size_t out_size, size_t max_len);

/* True when the whole text would be a valid record. Convenience for callers
 * that only need the yes/no (and for tests). */
int tbox_record_is_valid(const char *text);

/* ---- names (spec S8) ------------------------------------------------- */

/* Validates a name before it is ever written to disk: rejects the Windows
 * path separators and reserved characters, control characters, trailing
 * dots/spaces, the reserved device names (CON, PRN, ... COM1-LPT9), an empty
 * name, a name over 255 UTF-8 bytes, and anything that would escape a
 * directory ("..", an absolute path, a leading/trailing slash).
 *
 * Returns TBOX_REC_OK when the name is safe to use. */
TBoxRecordStatus tbox_name_validate(const char *name);

/* Replaces the characters S8 forbids with '_', trims trailing dots/spaces
 * and truncates to 255 UTF-8 bytes on a codepoint boundary, so a name that
 * arrives from Telegram becomes something safe to write. A name that cannot
 * be salvaged (empty, or a reserved device name) becomes "unnamed". UTF-8
 * input is assumed; invalid bytes are replaced, never copied through. */
void tbox_name_sanitize(const char *name, char *out, size_t out_size);

#endif /* TBOX_CATALOG_RECORD_H */
