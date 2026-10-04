/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * core/indexfile.c - the index.json mirror (see indexfile.h).
 *
 * Deliberately partial: names, ids, sizes and timestamps only. The sha256 and
 * the is_tbox verdict are re-derived from each caption on the next scan, so a
 * mirror that claimed to know them could be wrong in a way that matters.
 */

#define _CRT_SECURE_NO_WARNINGS   /* MSVC: strcpy / remove / rename warnings */

#include "indexfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

/* Refuse to load a mirror that claims more entries than this. It costs one
 * allocation per entry, so a corrupt length field would otherwise be an
 * out-of-memory bomb; the real ceiling is the account's message count. */
#define TBOX_INDEXFILE_MAX_ENTRIES 1000000LL

/* ---- write ----------------------------------------------------------------- */

int tbox_indexfile_write(const char *path, const tbox_archive_t *ar)
{
    char tmp[640];
    cJSON *root;
    cJSON *list;
    FILE *fp;
    size_t i;
    size_t len;
    char *text;

    if (path == NULL || ar == NULL)
        return -1;

    /* never build "<path>.tmp" with snprintf("%s.tmp"): gcc -Wformat-truncation
     * cannot prove it fits and -Wall -Werror would fail the build */
    len = strlen(path);
    if (len == 0 || len + 5 > sizeof tmp) {
        fprintf(stderr, "indexfile: path is too long.\n");
        return -1;
    }
    memcpy(tmp, path, len);
    memcpy(tmp + len, ".tmp", 5);

    root = cJSON_CreateObject();
    list = cJSON_CreateArray();
    if (root == NULL || list == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(list);
        return -1;
    }
    cJSON_AddNumberToObject(root, "v", TBOX_INDEXFILE_V);
    cJSON_AddItemToObject(root, "entries", list);

    for (i = 0; i < ar->count; i++) {
        const tbox_archive_entry_t *e = &ar->items[i];
        cJSON *item = cJSON_CreateObject();

        if (item == NULL) {
            cJSON_Delete(root);
            return -1;
        }
        cJSON_AddStringToObject(item, "name", e->name);
        cJSON_AddNumberToObject(item, "m", (double)e->message_id);
        cJSON_AddNumberToObject(item, "f", (double)e->file_id);
        cJSON_AddNumberToObject(item, "z", (double)e->size);
        cJSON_AddNumberToObject(item, "t", (double)e->mtime);
        cJSON_AddNumberToObject(item, "d", e->is_dir);
        cJSON_AddItemToArray(list, item);
    }

    text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == NULL)
        return -1;

    /* written by hand rather than with cJSON_Print to file: this keeps the
     * "\n"-terminated, atomically-replaced shape identical on every OS */
    fp = fopen(tmp, "wb");
    if (fp == NULL) {
        cJSON_free(text);
        fprintf(stderr, "indexfile: cannot write %s\n", tmp);
        return -1;
    }
    if (fwrite(text, 1, strlen(text), fp) != strlen(text)
        || fputc('\n', fp) == EOF || fclose(fp) != 0) {
        cJSON_free(text);
        remove(tmp);
        fprintf(stderr, "indexfile: cannot write %s\n", tmp);
        return -1;
    }
    cJSON_free(text);

    /* rename over the target: a reader sees the old file or the new one, never
     * a half-written one */
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        fprintf(stderr, "indexfile: cannot replace %s\n", path);
        return -1;
    }

    return 0;
}

/* ---- read ------------------------------------------------------------------ */

static long long json_num(const cJSON *obj, const char *key, long long fallback)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);

    if (!cJSON_IsNumber(item))
        return fallback;
    return (long long)item->valuedouble;
}

long long tbox_indexfile_read(const char *path, tbox_archive_t *ar)
{
    FILE *fp;
    cJSON *root;
    const cJSON *list;
    const cJSON *item;
    char *text;
    long size;
    long loaded = 0;

    if (path == NULL || ar == NULL)
        return -1;

    fp = fopen(path, "rb");
    if (fp == NULL)
        return -1;                     /* cold start, not a failure */

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    size = ftell(fp);
    if (size < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    /* +2: the trailing newline plus the NUL cJSON needs */
    if (size > 64LL * 1024LL * 1024LL) {
        fclose(fp);
        fprintf(stderr, "indexfile: %s is implausibly large; ignoring.\n", path);
        return -1;
    }

    text = malloc((size_t)size + 2);
    if (text == NULL) {
        fclose(fp);
        return -1;
    }
    size = (long)fread(text, 1, (size_t)size, fp);
    fclose(fp);
    text[size] = '\0';

    root = cJSON_Parse(text);
    free(text);
    if (root == NULL)
        return -1;

    if (!cJSON_IsObject(root) || json_num(root, "v", -1) != TBOX_INDEXFILE_V) {
        cJSON_Delete(root);
        return -1;
    }

    list = cJSON_GetObjectItem(root, "entries");
    if (!cJSON_IsArray(list)) {
        cJSON_Delete(root);
        return -1;
    }

    cJSON_ArrayForEach(item, list) {
        tbox_archive_entry_t e;
        const char *name;

        if (loaded >= TBOX_INDEXFILE_MAX_ENTRIES) {
            fprintf(stderr, "indexfile: %s claims too many entries; "
                            "ignoring the rest.\n", path);
            break;
        }

        if (!cJSON_IsObject(item))
            continue;

        name = cJSON_GetStringValue(cJSON_GetObjectItem(item, "name"));
        if (name == NULL)
            continue;                   /* unusable: skip it, keep the rest */

        /* bounded copy: add() normalizes and rejects, but it must never see
         * an unterminated buffer first */
        {
            size_t nlen = strlen(name);

            if (nlen >= sizeof e.name)
                continue;
            memset(&e, 0, sizeof e);
            memcpy(e.name, name, nlen + 1);
        }
        e.message_id = json_num(item, "m", TBOX_MSG_NONE);
        e.file_id = json_num(item, "f", 0);
        e.size = json_num(item, "z", 0);
        e.mtime = json_num(item, "t", -1);
        e.is_dir = (int)json_num(item, "d", 0);

        /* add() normalizes the name, so an invalid or duplicate one is simply
         * not counted: a hand-edited mirror degrades instead of breaking */
        if (tbox_archive_add(ar, &e) == 0)
            loaded++;
    }

    cJSON_Delete(root);

    return loaded;
}
