/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * core/index.c - the archive ledger: caption schema, path normalization,
 * implied directories and directory listing. Pure logic, no TDLib and no
 * filesystem, so tests/test_index.c can cover every rule.
 */

#define _CRT_SECURE_NO_WARNINGS   /* MSVC: no "unsafe" warnings for strncpy etc. */

#include "index.h"

#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

/* ---- path normalization ---------------------------------------------------- */

/*
 * v0.3 mapping.to_archive_name(): fold separators, drop empty and "."
 * segments, reject any whole ".." segment (reject, never collapse).
 */
int tbox_archive_name(const char *path, char *out, size_t size)
{
    char work[TBOX_ARCHIVE_MAX_NAME];
    const char *p;
    size_t used = 0;
    size_t i;

    if (path == NULL || out == NULL || size == 0)
        return -1;

    /* one pass over a private copy; strtok_r is POSIX-only, so the walk is
     * done by hand (portable to MSVC without a shim) */
    if (strlen(path) >= sizeof work)
        return -1;
    strcpy(work, path);
    for (i = 0; work[i] != '\0'; i++) {
        if (work[i] == '\\')
            work[i] = '/';
    }

    out[0] = '\0';

    for (p = work; *p != '\0'; ) {
        size_t seg_len;

        while (*p == '/')          /* skip empty segments */
            p++;
        if (*p == '\0')
            break;

        seg_len = strcspn(p, "/");

        if (seg_len == 1 && p[0] == '.')
            goto next;             /* drop "." segments */
        if (seg_len == 2 && p[0] == '.' && p[1] == '.')
            return -1;             /* traversal: reject, never collapse */

        if (used != 0) {
            if (used + 1 >= size)
                return -1;
            out[used++] = '/';
        }
        if (seg_len >= size - used)
            return -1;
        memcpy(out + used, p, seg_len);
        used += seg_len;
        out[used] = '\0';

    next:
        p += seg_len;
    }

    return 0;
}

/* ---- caption schema -------------------------------------------------------- */

/* json_get_string(): NULL unless the value is a string. */
static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    return cJSON_IsString(item) ? item->valuestring : NULL;
}

/* json_get_int(): 0 unless the value is a number that fits an int. */
static long long json_int(const cJSON *obj, const char *key, long long fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    if (!cJSON_IsNumber(item))
        return fallback;
    return (long long)item->valuedouble;
}

int tbox_caption_parse(const char *caption, tbox_archive_entry_t *out)
{
    cJSON *root;
    const char *type;
    const char *name;
    const char *sha;
    long long mtime;

    if (out == NULL || caption == NULL || caption[0] == '\0')
        return -1;

    root = cJSON_Parse(caption);
    if (root == NULL)
        return -1;                       /* foreign document */

    if (!cJSON_IsObject(root)
        || json_int(root, "v", -1) != TBOX_ARCHIVE_META_V) {
        cJSON_Delete(root);
        return -1;                       /* not ours (v0.3 rule: v must be 1) */
    }

    memset(out, 0, sizeof *out);
    out->message_id = TBOX_MSG_NONE;
    out->mtime = -1;

    type = json_str(root, "type");
    if (type == NULL)
        type = "file";                   /* v0.3 default */

    name = json_str(root, "name");
    if (name == NULL || name[0] == '\0') {
        cJSON_Delete(root);
        return -1;                       /* caller falls back to the document */
    }

    /* trim trailing separators on a private copy (name is const) */
    {
        char trimmed[TBOX_ARCHIVE_MAX_NAME];
        size_t len = strlen(name);

        while (len > 0 && name[len - 1] == '/')
            len--;
        if (len == 0 || len >= sizeof trimmed) {
            cJSON_Delete(root);
            return -1;
        }
        memcpy(trimmed, name, len);
        trimmed[len] = '\0';

        if (tbox_archive_name(trimmed, out->name, sizeof out->name) != 0) {
            cJSON_Delete(root);
            return -1;
        }
    }

    if (strcmp(type, "dir") == 0) {
        /* dir markers carry only v/type/name */
        out->is_dir = 1;
        out->is_tbox = 1;
        cJSON_Delete(root);
        return 0;
    }

    out->is_dir = 0;
    out->size = json_int(root, "size", 0);
    mtime = json_int(root, "mtime", -1);
    sha = json_str(root, "sha256");
    if (sha != NULL && strlen(sha) < sizeof out->sha256)
        strcpy(out->sha256, sha);
    /* v0.3: is_tbox == a file with both sha256 and an integer mtime */
    out->mtime = mtime;
    out->is_tbox = (sha != NULL && sha[0] != '\0' && mtime >= 0);

    cJSON_Delete(root);
    return 0;
}

char *tbox_caption_build(const tbox_archive_entry_t *entry)
{
    cJSON *root;
    char *json;

    if (entry == NULL)
        return NULL;

    root = cJSON_CreateObject();
    if (root == NULL)
        return NULL;

    /* key order is the v0.3 insertion order and must stay that way */
    cJSON_AddNumberToObject(root, "v", TBOX_ARCHIVE_META_V);
    cJSON_AddStringToObject(root, "type", entry->is_dir ? "dir" : "file");
    cJSON_AddStringToObject(root, "name", entry->name);
    if (!entry->is_dir) {
        cJSON_AddNumberToObject(root, "size", (double)entry->size);
        cJSON_AddNumberToObject(root, "mtime", (double)entry->mtime);
        cJSON_AddStringToObject(root, "sha256", entry->sha256);
    }

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    /* cJSON emits UTF-8 raw where Python's json.dumps escaped non-ASCII as
     * \uXXXX; both parse identically in the old client. */
    return json;
}

/* ---- the index ------------------------------------------------------------ */

void tbox_archive_init(tbox_archive_t *ar)
{
    if (ar != NULL)
        memset(ar, 0, sizeof *ar);
}

void tbox_archive_free(tbox_archive_t *ar)
{
    if (ar == NULL)
        return;
    free(ar->items);
    memset(ar, 0, sizeof *ar);
}

static int archive_reserve(tbox_archive_t *ar, size_t want)
{
    tbox_archive_entry_t *grown;
    size_t cap;

    if (want <= ar->capacity)
        return 0;

    cap = ar->capacity != 0 ? ar->capacity * 2 : 16;
    while (cap < want)
        cap *= 2;

    grown = realloc(ar->items, cap * sizeof *grown);
    if (grown == NULL)
        return -1;

    ar->items = grown;
    ar->capacity = cap;

    return 0;
}

const tbox_archive_entry_t *tbox_archive_find(tbox_archive_t *ar,
                                              const char *path)
{
    char name[TBOX_ARCHIVE_MAX_NAME];
    size_t i;

    if (ar == NULL || tbox_archive_name(path, name, sizeof name) != 0)
        return NULL;

    for (i = 0; i < ar->count; i++) {
        if (strcmp(ar->items[i].name, name) == 0)
            return &ar->items[i];
    }

    return NULL;
}

int tbox_archive_add(tbox_archive_t *ar, const tbox_archive_entry_t *entry)
{
    tbox_archive_entry_t copy;
    size_t i;

    if (ar == NULL || entry == NULL)
        return -1;

    /* callers may hand us a client path, so normalize before storing */
    copy = *entry;
    copy.name[sizeof copy.name - 1] = '\0';
    if (tbox_archive_name(entry->name, copy.name, sizeof copy.name) != 0)
        return -1;

    for (i = 0; i < ar->count; i++) {
        if (strcmp(ar->items[i].name, copy.name) == 0) {
            /* newest wins, so upload-then-delete replacement resolves
             * deterministically; implied dirs may need re-adding */
            ar->items[i] = copy;
            ar->materialized = 0;
            return 0;
        }
    }

    if (archive_reserve(ar, ar->count + 1) != 0)
        return -1;

    ar->items[ar->count] = copy;
    ar->count++;
    ar->materialized = 0;

    return 0;
}

/* Compare by name, byte order. */
static int cmp_entry(const void *a, const void *b)
{
    const tbox_archive_entry_t *ea = a;
    const tbox_archive_entry_t *eb = b;

    return strcmp(ea->name, eb->name);
}

void tbox_archive_sort(tbox_archive_t *ar)
{
    if (ar == NULL || ar->count < 2)
        return;
    qsort(ar->items, ar->count, sizeof *ar->items, cmp_entry);
}

void tbox_archive_materialize(tbox_archive_t *ar)
{
    size_t i;

    if (ar == NULL || ar->materialized)
        return;

    /* i < ar->count, not == : the list grows as implied dirs are added, and
     * those need their own prefixes materialized too. The name is copied out
     * first because add() may realloc the item array. */
    for (i = 0; i < ar->count; i++) {
        char name[TBOX_ARCHIVE_MAX_NAME];
        size_t k;

        strcpy(name, ar->items[i].name);

        /* every "/" ends one prefix: "a/b/c" -> "a", then "a/b" */
        for (k = 1; name[k] != '\0'; k++) {
            tbox_archive_entry_t dir;

            if (name[k] != '/')
                continue;

            name[k] = '\0';
            if (tbox_archive_find(ar, name) == NULL) {
                memset(&dir, 0, sizeof dir);
                strcpy(dir.name, name);
                dir.message_id = TBOX_MSG_NONE;
                dir.is_dir = 1;
                dir.mtime = -1;
                (void)tbox_archive_add(ar, &dir);
            }
            name[k] = '/';
        }
    }

    ar->materialized = 1;
}

int tbox_archive_stat(tbox_archive_t *ar, const char *path,
                      tbox_archive_entry_t *out)
{
    char name[TBOX_ARCHIVE_MAX_NAME];
    char prefix[TBOX_ARCHIVE_MAX_NAME + 2];
    const tbox_archive_entry_t *hit;
    size_t i;

    if (ar == NULL || out == NULL
        || tbox_archive_name(path, name, sizeof name) != 0)
        return -1;

    /* the archive root always exists as a directory */
    if (name[0] == '\0') {
        memset(out, 0, sizeof *out);
        out->message_id = TBOX_MSG_NONE;
        out->is_dir = 1;
        out->mtime = -1;
        return 0;
    }

    hit = tbox_archive_find(ar, name);
    if (hit != NULL) {
        *out = *hit;
        return 0;
    }

    /* implied directory? */
    if (strlen(name) + 2 > sizeof prefix)
        return -1;
    strcpy(prefix, name);
    strcat(prefix, "/");

    for (i = 0; i < ar->count; i++) {
        if (strncmp(ar->items[i].name, prefix, strlen(prefix)) == 0) {
            memset(out, 0, sizeof *out);
            strcpy(out->name, name);
            out->message_id = TBOX_MSG_NONE;
            out->is_dir = 1;
            out->mtime = -1;
            return 0;
        }
    }

    return -1;
}

/* One listed child: the first path segment of some entry. `seg` points at
 * that segment inside entry->name (valid while the index is not modified). */
typedef struct
{
    const tbox_archive_entry_t *entry;
    const char *seg;
    size_t seg_len;
    int is_dir;

} child_ref_t;

static int cmp_child(const void *a, const void *b)
{
    const child_ref_t *ca = a;
    const child_ref_t *cb = b;
    size_t n = ca->seg_len < cb->seg_len ? ca->seg_len : cb->seg_len;
    int diff = strncmp(ca->seg, cb->seg, n);

    if (diff != 0)
        return diff;
    if (ca->seg_len != cb->seg_len)
        return ca->seg_len < cb->seg_len ? -1 : 1;

    return 0;
}

int tbox_archive_list(tbox_archive_t *ar, const char *path,
                      tbox_archive_entry_t **out, size_t *cap)
{
    char name[TBOX_ARCHIVE_MAX_NAME];
    char prefix[TBOX_ARCHIVE_MAX_NAME + 2];
    child_ref_t *kids = NULL;
    size_t kid_count = 0;
    size_t kid_cap = 0;
    size_t prefix_len;
    size_t i;
    size_t n;

    if (ar == NULL || out == NULL || cap == NULL)
        return -1;
    *out = NULL;
    *cap = 0;

    if (tbox_archive_name(path, name, sizeof name) != 0)
        return -1;

    tbox_archive_materialize(ar);

    /* the directory must exist */
    {
        tbox_archive_entry_t self;

        if (tbox_archive_stat(ar, name, &self) != 0)
            return -2;
        if (!self.is_dir)
            return -2;
    }

    prefix[0] = '\0';
    if (name[0] != '\0') {
        if (strlen(name) + 2 > sizeof prefix)
            return -1;
        strcpy(prefix, name);
        strcat(prefix, "/");
    }
    prefix_len = strlen(prefix);

    for (i = 0; i < ar->count; i++) {
        const tbox_archive_entry_t *e = &ar->items[i];
        const char *rest;
        const char *slash;
        size_t seg_len;
        int is_dir;
        size_t k;
        int seen = 0;

        if (strncmp(e->name, prefix, prefix_len) != 0)
            continue;                        /* not under `path` */
        rest = e->name + prefix_len;
        if (rest[0] == '\0')
            continue;                        /* the dir marker itself */
        if (strcmp(rest, ".") == 0 || strcmp(rest, "..") == 0)
            continue;

        slash = strchr(rest, '/');
        seg_len = slash != NULL ? (size_t)(slash - rest) : strlen(rest);
        /* an entry further down makes its first segment a directory */
        is_dir = (slash != NULL) ? 1 : e->is_dir;

        for (k = 0; k < kid_count; k++) {
            if (kids[k].seg_len == seg_len
                && strncmp(kids[k].seg, rest, seg_len) == 0) {
                seen = 1;
                break;
            }
        }
        if (seen)
            continue;

        if (kid_count == kid_cap) {
            child_ref_t *grown;

            kid_cap = kid_cap != 0 ? kid_cap * 2 : 16;
            grown = realloc(kids, kid_cap * sizeof *grown);
            if (grown == NULL) {
                free(kids);
                return -1;
            }
            kids = grown;
        }

        kids[kid_count].entry = e;
        kids[kid_count].seg = rest;
        kids[kid_count].seg_len = seg_len;
        kids[kid_count].is_dir = is_dir;
        kid_count++;
    }

    /* empty directory: answer "." and ".." so clients do not complain */
    n = kid_count != 0 ? kid_count : 2;
    *out = calloc(n, sizeof **out);
    if (*out == NULL) {
        free(kids);
        return -1;
    }
    *cap = n;

    if (kid_count == 0) {
        for (i = 0; i < 2; i++) {
            strcpy((*out)[i].name, i == 0 ? "." : "..");
            (*out)[i].message_id = TBOX_MSG_NONE;
            (*out)[i].is_dir = 1;
            (*out)[i].mtime = 0;
        }
        free(kids);
        return 0;
    }

    qsort(kids, kid_count, sizeof *kids, cmp_child);

    for (i = 0; i < kid_count; i++) {
        tbox_archive_entry_t *dst = &(*out)[i];
        const tbox_archive_entry_t *dirent = NULL;

        memset(dst, 0, sizeof *dst);
        if (prefix_len != 0)
            memcpy(dst->name, prefix, prefix_len);
        memcpy(dst->name + prefix_len, kids[i].seg, kids[i].seg_len);
        dst->name[prefix_len + kids[i].seg_len] = '\0';
        if (kids[i].is_dir) {
            /* an explicit dir marker owns the child, not the file that made
             * it look like a directory: look it up by name */
            dirent = tbox_archive_find(ar, dst->name);

            dst->message_id = dirent != NULL ? dirent->message_id : TBOX_MSG_NONE;
            dst->file_id = dirent != NULL ? dirent->file_id : 0;
        } else {
            dst->message_id = kids[i].entry->message_id;
            dst->file_id = kids[i].entry->file_id;
        }
        dst->is_dir = kids[i].is_dir;
        dst->size = kids[i].is_dir ? 0 : kids[i].entry->size;
        /*
         * A directory's date is whatever its own entry says: the explicit
         * marker's date when there is a marker, and the entry materialize made
         * when there is not. Both already use the archive's own convention for
         * "unknown", so nothing is invented here - and a listing that claimed
         * Jan 1970 for a folder with a real date on it would just be wrong.
         */
        dst->mtime = kids[i].is_dir
                ? (dirent != NULL ? dirent->mtime : -1)
                : kids[i].entry->mtime;
        strcpy(dst->sha256, kids[i].is_dir ? "" : kids[i].entry->sha256);
        dst->is_tbox = kids[i].is_dir ? 0 : kids[i].entry->is_tbox;
    }

    free(kids);
    return 0;
}