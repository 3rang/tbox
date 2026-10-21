/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * record.c — parse and build the tbx1 record, and validate names (spec 7.2,
 * S8). Pure: no TDLib, no filesystem, no SQLite, no allocation beyond the
 * cJSON tree it frees before returning.
 *
 * The two rules that matter most here:
 *   - a managed prefix with a broken body is NOT silently repaired; it comes
 *     back as a specific error so the caller can mark the message
 *     bad_record=1 and show it in the Inbox/Notes rather than lose it;
 *   - building is deterministic: fixed key order, no spaces, optional keys
 *     omitted, so the same fields always produce the identical caption.
 */
#include "catalog/record.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* ---- small helpers --------------------------------------------------- */

/* lowercase hex of exactly n characters, or false. */
static int is_hex_lower(const char *s, size_t n) {
    size_t i;
    if (s == NULL || strlen(s) != n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

/* Copies src into dst (cap bytes) and reports whether it fit. */
static int copy_bounded(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n + 1 > cap) {
        return 0;
    }
    memcpy(dst, src, n + 1);
    return 1;
}

/* Number of bytes in the UTF-8 sequence starting at s[0], clamped to the
 * remaining length. A stray continuation/lead byte counts as 1 so a walk
 * over a malformed string still terminates. */
static size_t utf8_seq_len(const unsigned char *s, size_t remaining) {
    unsigned char c = s[0];
    size_t need = 1;
    if ((c & 0x80) == 0x00) {
        need = 1;
    } else if ((c & 0xE0) == 0xC0) {
        need = 2;
    } else if ((c & 0xF0) == 0xE0) {
        need = 3;
    } else if ((c & 0xF8) == 0xF0) {
        need = 4;
    }
    if (need > remaining) {
        return 1; /* truncated sequence: step one byte, do not overshoot */
    }
    for (size_t i = 1; i < need; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            return 1; /* not a real continuation: treat the lead as stray */
        }
    }
    return need;
}

/* ---- names (spec S8) ------------------------------------------------- */

/* Windows reserved device names, case-insensitive, with or without a
 * well-known extension. CON.txt is still CON. */
static int is_reserved_device(const char *name) {
    static const char *const reserved[] = {
        "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
        "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
        "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };
    char stem[8];
    size_t i = 0;
    /* take characters up to the first dot */
    while (name[i] != '\0' && name[i] != '.' && i < sizeof stem - 1) {
        stem[i] = name[i];
        i++;
    }
    stem[i] = '\0';
    /* uppercase-compare */
    for (i = 0; stem[i] != '\0'; i++) {
        if (stem[i] >= 'a' && stem[i] <= 'z') {
            stem[i] = (char)(stem[i] - 'a' + 'A');
        }
    }
    for (size_t r = 0; r < sizeof reserved / sizeof reserved[0]; r++) {
        if (strcmp(stem, reserved[r]) == 0) {
            return 1;
        }
    }
    return 0;
}

TBoxRecordStatus tbox_name_validate(const char *name) {
    if (name == NULL) {
        return TBOX_REC_E_ARGS;
    }
    if (name[0] == '\0') {
        return TBOX_REC_E_NAME;
    }
    if (strlen(name) > TBOX_NAME_MAX_BYTES) {
        return TBOX_REC_E_NAME;
    }
    /* absolute paths, parent escapes and separators, in any position */
    if (name[0] == '/' || name[0] == '\\') {
        return TBOX_REC_E_NAME;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return TBOX_REC_E_NAME;
    }
    if (strstr(name, "..") != NULL) {
        return TBOX_REC_E_NAME;
    }

    for (const unsigned char *p = (const unsigned char *)name; *p != '\0';) {
        size_t len = utf8_seq_len(p, strlen((const char *)p));
        if (len == 1) {
            unsigned char c = p[0];
            /* the forbidden set: control chars and \ / : * ? " < > | */
            if (c < 0x20 || c == 0x7F) {
                return TBOX_REC_E_NAME;
            }
            switch (c) {
            case '\\':
            case '/':
            case ':':
            case '*':
            case '?':
            case '"':
            case '<':
            case '>':
            case '|':
                return TBOX_REC_E_NAME;
            default:
                break;
            }
        }
        p += len;
    }

    /* trailing dot or space is silently dropped by Windows, so refuse it */
    {
        size_t n = strlen(name);
        if (name[n - 1] == '.' || name[n - 1] == ' ') {
            return TBOX_REC_E_NAME;
        }
    }
    if (is_reserved_device(name)) {
        return TBOX_REC_E_NAME;
    }
    return TBOX_REC_OK;
}

void tbox_name_sanitize(const char *name, char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (name == NULL) {
        return;
    }

    size_t used = 0;
    const unsigned char *p = (const unsigned char *)name;
    while (*p != '\0') {
        size_t len = utf8_seq_len(p, strlen((const char *)p));
        if (len == 1) {
            unsigned char c = p[0];
            int bad = (c < 0x20 || c == 0x7F);
            switch (c) {
            case '\\':
            case '/':
            case ':':
            case '*':
            case '?':
            case '"':
            case '<':
            case '>':
            case '|':
                bad = 1;
                break;
            default:
                break;
            }
            if (bad) {
                if (used + 1 + 1 > out_size) {
                    break;
                }
                out[used++] = '_';
                p += 1;
                continue;
            }
        }
        /* keep the whole codepoint, or stop if it would not fit */
        if (used + len + 1 > out_size) {
            break;
        }
        /* keep within the spec's byte budget, on a codepoint boundary */
        if (used + len > TBOX_NAME_MAX_BYTES) {
            break;
        }
        memcpy(out + used, p, len);
        used += len;
        p += len;
    }
    out[used] = '\0';

    /* trim trailing dots and spaces (Windows would drop them anyway) */
    while (used > 0 && (out[used - 1] == '.' || out[used - 1] == ' ')) {
        out[--used] = '\0';
    }
    /* leading dots would create hidden files / a bare "." */
    size_t start = 0;
    while (out[start] == '.') {
        start++;
    }
    if (start > 0) {
        memmove(out, out + start, used - start + 1);
        used -= start;
    }

    if (used == 0 || is_reserved_device(out) != 0) {
        /* nothing usable survived: give it a safe placeholder */
        copy_bounded(out, out_size, "unnamed");
    }
}

/* ---- parsing (spec 7.2) ---------------------------------------------- */

/* Reads a required string field and validates it with `check`. */
static TBoxRecordStatus required_hex(const cJSON *o, const char *key, char *dst,
                                     size_t cap, size_t hexlen) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(o, key);
    if (item == NULL) {
        return TBOX_REC_E_MISSING;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return TBOX_REC_E_TYPE;
    }
    if (!is_hex_lower(item->valuestring, hexlen)) {
        return TBOX_REC_E_ID;
    }
    if (!copy_bounded(dst, cap, item->valuestring)) {
        return TBOX_REC_E_ID;
    }
    return TBOX_REC_OK;
}

/* Reads an optional integer; absent is fine, present must be a number. */
static TBoxRecordStatus optional_int(const cJSON *o, const char *key, int *has,
                                     long long *value) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(o, key);
    if (item == NULL) {
        *has = 0;
        return TBOX_REC_OK;
    }
    if (!cJSON_IsNumber(item)) {
        return TBOX_REC_E_TYPE;
    }
    *has = 1;
    *value = (long long)item->valuedouble;
    return TBOX_REC_OK;
}

TBoxRecordStatus tbox_record_parse(const char *text, TBoxRecord *out) {
    if (text == NULL || out == NULL) {
        return TBOX_REC_E_ARGS;
    }
    size_t prefix = sizeof TBOX_RECORD_PREFIX - 1;
    if (strncmp(text, TBOX_RECORD_PREFIX, prefix) != 0) {
        return TBOX_REC_E_NOPREFIX;
    }

    const char *body = text + prefix;
    /* parse exactly the bytes we were given, and require nothing to trail */
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(body, strlen(body) + 1, &end, 1);
    if (root == NULL) {
        return TBOX_REC_E_SYNTAX;
    }
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return TBOX_REC_E_SYNTAX;
    }

    TBoxRecord rec;
    memset(&rec, 0, sizeof rec);
    TBoxRecordStatus st = TBOX_REC_OK;

    /* v: required, must be 1 */
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "v");
    if (v == NULL) {
        st = TBOX_REC_E_VERSION;
        goto done;
    }
    if (!cJSON_IsNumber(v)) {
        st = TBOX_REC_E_TYPE;
        goto done;
    }
    if ((long long)v->valuedouble != TBOX_RECORD_VERSION) {
        st = TBOX_REC_E_VERSION;
        goto done;
    }
    rec.version = TBOX_RECORD_VERSION;

    /* k: required, "f" or "d" */
    {
        const cJSON *k = cJSON_GetObjectItemCaseSensitive(root, "k");
        if (k == NULL) {
            st = TBOX_REC_E_MISSING;
            goto done;
        }
        if (!cJSON_IsString(k) || k->valuestring == NULL) {
            st = TBOX_REC_E_TYPE;
            goto done;
        }
        if (strcmp(k->valuestring, "f") == 0) {
            rec.kind = 'f';
        } else if (strcmp(k->valuestring, "d") == 0) {
            rec.kind = 'd';
        } else {
            st = TBOX_REC_E_TYPE;
            goto done;
        }
    }

    /* o: required 32 hex */
    st = required_hex(root, "o", rec.object_id, sizeof rec.object_id,
                      TBOX_ID_HEX_LEN);
    if (st != TBOX_REC_OK) {
        goto done;
    }

    /* p: required, "root" or 32 hex */
    {
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(root, "p");
        if (p == NULL) {
            st = TBOX_REC_E_MISSING;
            goto done;
        }
        if (!cJSON_IsString(p) || p->valuestring == NULL) {
            st = TBOX_REC_E_TYPE;
            goto done;
        }
        if (strcmp(p->valuestring, "root") != 0 &&
            !is_hex_lower(p->valuestring, TBOX_ID_HEX_LEN)) {
            st = TBOX_REC_E_ID;
            goto done;
        }
        if (!copy_bounded(rec.parent, sizeof rec.parent, p->valuestring)) {
            st = TBOX_REC_E_ID;
            goto done;
        }
    }

    /* n: required, S8-valid */
    {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(root, "n");
        if (n == NULL) {
            st = TBOX_REC_E_MISSING;
            goto done;
        }
        if (!cJSON_IsString(n) || n->valuestring == NULL) {
            st = TBOX_REC_E_TYPE;
            goto done;
        }
        if (tbox_name_validate(n->valuestring) != TBOX_REC_OK) {
            st = TBOX_REC_E_NAME;
            goto done;
        }
        if (!copy_bounded(rec.name, sizeof rec.name, n->valuestring)) {
            st = TBOX_REC_E_NAME;
            goto done;
        }
    }

    /* r: required integer >= 1 */
    {
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "r");
        if (r == NULL) {
            st = TBOX_REC_E_MISSING;
            goto done;
        }
        if (!cJSON_IsNumber(r)) {
            st = TBOX_REC_E_TYPE;
            goto done;
        }
        rec.rev = (long long)r->valuedouble;
        if (rec.rev < 1) {
            st = TBOX_REC_E_RANGE;
            goto done;
        }
    }

    /* s: required for files, optional for folders; negative is invalid */
    st = optional_int(root, "s", &rec.has_size, &rec.size);
    if (st != TBOX_REC_OK) {
        goto done;
    }
    if (rec.has_size && rec.size < 0) {
        st = TBOX_REC_E_RANGE;
        goto done;
    }
    if (rec.kind == 'f' && !rec.has_size) {
        st = TBOX_REC_E_MISSING;
        goto done;
    }

    /* h: optional 64 hex */
    {
        const cJSON *h = cJSON_GetObjectItemCaseSensitive(root, "h");
        if (h != NULL) {
            if (!cJSON_IsString(h) || h->valuestring == NULL) {
                st = TBOX_REC_E_TYPE;
                goto done;
            }
            if (!is_hex_lower(h->valuestring, TBOX_SHA256_HEX_LEN)) {
                st = TBOX_REC_E_ID;
                goto done;
            }
            if (!copy_bounded(rec.sha256, sizeof rec.sha256, h->valuestring)) {
                st = TBOX_REC_E_ID;
                goto done;
            }
            rec.has_sha256 = 1;
        }
    }

    /* m, t: optional integers */
    st = optional_int(root, "m", &rec.has_mtime, &rec.mtime);
    if (st != TBOX_REC_OK) {
        goto done;
    }
    st = optional_int(root, "t", &rec.has_trashed, &rec.trashed_at);
    if (st != TBOX_REC_OK) {
        goto done;
    }
    if (rec.has_trashed && rec.trashed_at < 0) {
        st = TBOX_REC_E_RANGE;
        goto done;
    }

    /* q: optional 16 hex */
    {
        const cJSON *q = cJSON_GetObjectItemCaseSensitive(root, "q");
        if (q != NULL) {
            if (!cJSON_IsString(q) || q->valuestring == NULL) {
                st = TBOX_REC_E_TYPE;
                goto done;
            }
            if (!is_hex_lower(q->valuestring, TBOX_OP_HEX_LEN)) {
                st = TBOX_REC_E_ID;
                goto done;
            }
            if (!copy_bounded(rec.op_id, sizeof rec.op_id, q->valuestring)) {
                st = TBOX_REC_E_ID;
                goto done;
            }
            rec.has_op = 1;
        }
    }

    /* src: optional string (a message id, kept as text) */
    {
        const cJSON *src = cJSON_GetObjectItemCaseSensitive(root, "src");
        if (src != NULL) {
            if (!cJSON_IsString(src) || src->valuestring == NULL) {
                st = TBOX_REC_E_TYPE;
                goto done;
            }
            if (src->valuestring[0] == '\0' ||
                strlen(src->valuestring) > TBOX_MSGID_MAX_BYTES) {
                st = TBOX_REC_E_ID;
                goto done;
            }
            if (!copy_bounded(rec.src, sizeof rec.src, src->valuestring)) {
                st = TBOX_REC_E_ID;
                goto done;
            }
            rec.has_src = 1;
        }
    }

    *out = rec;

done:
    cJSON_Delete(root);
    return st;
}

int tbox_record_is_valid(const char *text) {
    TBoxRecord rec;
    return tbox_record_parse(text, &rec) == TBOX_REC_OK;
}

/* ---- building (spec 7.2) --------------------------------------------- */

TBoxRecordStatus tbox_record_build(const TBoxRecord *rec, char *out,
                                   size_t out_size, size_t max_len) {
    if (rec == NULL || out == NULL || out_size == 0) {
        return TBOX_REC_E_ARGS;
    }
    if (rec->kind != 'f' && rec->kind != 'd') {
        return TBOX_REC_E_ARGS;
    }
    if (!is_hex_lower(rec->object_id, TBOX_ID_HEX_LEN)) {
        return TBOX_REC_E_ARGS;
    }
    if (strcmp(rec->parent, "root") != 0 &&
        !is_hex_lower(rec->parent, TBOX_ID_HEX_LEN)) {
        return TBOX_REC_E_ARGS;
    }
    if (tbox_name_validate(rec->name) != TBOX_REC_OK) {
        return TBOX_REC_E_NAME;
    }
    if (rec->rev < 1) {
        return TBOX_REC_E_ARGS;
    }

    /* keys are inserted in exactly the spec's order; cJSON preserves the
     * insertion order when printing, which is what makes output stable */
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return TBOX_REC_E_ARGS;
    }
    cJSON_AddNumberToObject(o, "v", TBOX_RECORD_VERSION);
    cJSON_AddStringToObject(o, "k", rec->kind == 'f' ? "f" : "d");
    cJSON_AddStringToObject(o, "o", rec->object_id);
    cJSON_AddStringToObject(o, "p", rec->parent);
    cJSON_AddStringToObject(o, "n", rec->name);
    cJSON_AddNumberToObject(o, "r", (double)rec->rev);
    if (rec->has_size) {
        cJSON_AddNumberToObject(o, "s", (double)rec->size);
    }
    if (rec->has_sha256) {
        cJSON_AddStringToObject(o, "h", rec->sha256);
    }
    if (rec->has_mtime) {
        cJSON_AddNumberToObject(o, "m", (double)rec->mtime);
    }
    if (rec->has_trashed) {
        cJSON_AddNumberToObject(o, "t", (double)rec->trashed_at);
    }
    if (rec->has_op) {
        cJSON_AddStringToObject(o, "q", rec->op_id);
    }
    if (rec->has_src) {
        cJSON_AddStringToObject(o, "src", rec->src);
    }

    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (json == NULL) {
        return TBOX_REC_E_ARGS;
    }

    size_t prefix = sizeof TBOX_RECORD_PREFIX - 1;
    size_t total = prefix + strlen(json);
    if (max_len != 0 && total > max_len) {
        free(json);
        return TBOX_REC_E_TOO_LONG;
    }
    if (total + 1 > out_size) {
        free(json);
        return TBOX_REC_E_ARGS;
    }
    memcpy(out, TBOX_RECORD_PREFIX, prefix);
    memcpy(out + prefix, json, strlen(json) + 1);
    free(json);
    return TBOX_REC_OK;
}
