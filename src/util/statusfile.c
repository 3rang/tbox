/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * util/statusfile.c - status.json: atomic write, tolerant read.
 * Pure file + JSON work, so tests/test_statusfile.c covers it offline.
 */

#define _CRT_SECURE_NO_WARNINGS   /* MSVC: fopen/sprintf are fine here */

#ifndef _WIN32
/* gmtime_r + fsync + fileno under a strict -std=c17 compile (see
 * util/lockfile.c for the same block and why it must come first). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include "statusfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cJSON.h>

#include "util/datadir.h"
#include "util/lockfile.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

/* ---- states --------------------------------------------------------------- */

const char *tbox_state_name(tbox_state_t state)
{
    switch (state) {
    case TBOX_STATE_STARTING:    return "starting";
    case TBOX_STATE_QR_REQUIRED: return "qr_required";
    case TBOX_STATE_AUTHORIZING: return "authorizing";
    case TBOX_STATE_READY:       return "ready";
    case TBOX_STATE_STOPPED:     return "stopped";
    case TBOX_STATE_ERROR:       return "error";
    default:                     return "unknown";
    }
}

tbox_state_t tbox_state_from_name(const char *name)
{
    if (name == NULL)
        return TBOX_STATE_UNKNOWN;

    /* keep the table and the strings together: no way to drift apart */
    for (int i = TBOX_STATE_STARTING; i <= TBOX_STATE_ERROR; i++) {
        if (strcmp(tbox_state_name((tbox_state_t)i), name) == 0)
            return (tbox_state_t)i;
    }

    return TBOX_STATE_UNKNOWN;
}

int tbox_state_is_running(tbox_state_t state)
{
    switch (state) {
    case TBOX_STATE_STARTING:
    case TBOX_STATE_QR_REQUIRED:
    case TBOX_STATE_AUTHORIZING:
    case TBOX_STATE_READY:
        return 1;
    default:
        return 0;
    }
}

/* ---- struct helpers ------------------------------------------------------- */

int tbox_status_path(char *out, size_t size, const char *root)
{
    return tbox_datadir_join(out, size, root, "status.json");
}

void tbox_status_init(tbox_status_t *status)
{
    if (status == NULL)
        return;

    memset(status, 0, sizeof *status);
    status->state = TBOX_STATE_STARTING;
    status->pid = tbox_pid_self();
    tbox_status_now(status->since, sizeof status->since);
}

void tbox_status_now(char *buf, size_t size)
{
    const time_t now = time(NULL);
    const struct tm *utc;

    if (buf == NULL || size == 0)
        return;

    buf[0] = '\0';

#ifdef _WIN32
    utc = gmtime(&now);            /* MSVC has no gmtime_r */
#else
    utc = gmtime_r(&now, &(struct tm){ 0 });
#endif
    if (utc == NULL)
        return;

    if (strftime(buf, size, "%Y-%m-%dT%H:%M:%SZ", utc) == 0)
        buf[0] = '\0';
}

/* The string value of a field, or NULL when it is absent / not a string. */
static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static long long json_int(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);

    return cJSON_IsNumber(item) ? (long long)item->valuedouble : 0;
}

/* Copy a JSON string field into a fixed buffer, truncating if needed;
 * missing (or not a string) = "". Plain memcpy, so no dependence on how
 * the compiler reasons about snprintf truncation. */
static void json_copy(char *dst, size_t size, const cJSON *obj, const char *key)
{
    const char *value = json_str(obj, key);
    size_t len;

    dst[0] = '\0';
    if (value == NULL)
        return;

    len = strlen(value);
    if (len >= size)
        len = size - 1;
    memcpy(dst, value, len);
    dst[len] = '\0';
}

/* ---- write ---------------------------------------------------------------- */

static int replace_file(const char *tmp, const char *path)
{
#ifdef _WIN32
    /* rename() does not overwrite on MSVC */
    return MoveFileExA(tmp, path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(tmp, path) == 0 ? 0 : -1;
#endif
}

int tbox_status_write(const char *path, const tbox_status_t *status)
{
    char tmp[512];
    cJSON *root;
    char *json;
    FILE *fp;
    size_t len;

    if (path == NULL || status == NULL)
        return -1;

    /* "<path>.tmp": length-checked memcpy (snprintf of "%s.tmp" is a
     * -Wformat-truncation candidate on gcc) */
    if (strlen(path) + sizeof ".tmp" > sizeof tmp)
        return -1;
    memcpy(tmp, path, strlen(path));
    memcpy(tmp + strlen(path), ".tmp", sizeof ".tmp");

    root = cJSON_CreateObject();
    if (root == NULL)
        return -1;

    cJSON_AddNumberToObject(root, "v", TBOX_STATUS_V);
    cJSON_AddStringToObject(root, "state", tbox_state_name(status->state));
    cJSON_AddStringToObject(root, "user", status->user);
    cJSON_AddStringToObject(root, "phone", status->phone);
    cJSON_AddStringToObject(root, "since", status->since);
    cJSON_AddStringToObject(root, "transport", status->transport);
    cJSON_AddStringToObject(root, "endpoint", status->endpoint);
    cJSON_AddNumberToObject(root, "indexed", (double)status->indexed);
    cJSON_AddNumberToObject(root, "cache_bytes", (double)status->cache_bytes);
    cJSON_AddNumberToObject(root, "pid", (double)status->pid);
    if (status->error[0] != '\0')
        cJSON_AddStringToObject(root, "error", status->error);
    else
        cJSON_AddNullToObject(root, "error");

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL)
        return -1;

    fp = fopen(tmp, "wb");
    if (fp == NULL) {
        cJSON_free(json);
        return -1;
    }

    len = strlen(json);
    if (fwrite(json, 1, len, fp) != len || fputc('\n', fp) == EOF) {
        (void)fclose(fp);
        (void)remove(tmp);
        cJSON_free(json);
        return -1;
    }
    cJSON_free(json);

    /* flush to the device first: a rename over unsynced data can survive a
     * power cut as an empty file */
    if (fflush(fp) != 0) {
        (void)fclose(fp);
        (void)remove(tmp);
        return -1;
    }
#ifdef _WIN32
    (void)_commit(_fileno(fp));
#else
    (void)fsync(fileno(fp));
#endif
    if (fclose(fp) != 0) {
        (void)remove(tmp);
        return -1;
    }

    if (replace_file(tmp, path) != 0) {
        (void)remove(tmp);
        return -1;
    }

    return 0;
}

/* ---- read ----------------------------------------------------------------- */

int tbox_status_read(const char *path, tbox_status_t *out)
{
    char buf[1024];
    FILE *fp;
    size_t got;
    cJSON *root;

    if (path == NULL || out == NULL)
        return -1;

    fp = fopen(path, "rb");
    if (fp == NULL)
        return -1;

    got = fread(buf, 1, sizeof buf - 1, fp);
    (void)fclose(fp);
    buf[got] = '\0';

    root = cJSON_Parse(buf);
    if (root == NULL)
        return -1;                      /* truncated or corrupt */

    if (!cJSON_IsObject(root)
        || json_int(root, "v") != TBOX_STATUS_V) {
        cJSON_Delete(root);
        return -1;
    }

    memset(out, 0, sizeof *out);
    out->state = tbox_state_from_name(json_str(root, "state"));
    json_copy(out->user, sizeof out->user, root, "user");
    json_copy(out->phone, sizeof out->phone, root, "phone");
    json_copy(out->since, sizeof out->since, root, "since");
    json_copy(out->transport, sizeof out->transport, root, "transport");
    json_copy(out->endpoint, sizeof out->endpoint, root, "endpoint");
    json_copy(out->error, sizeof out->error, root, "error");
    out->indexed = json_int(root, "indexed");
    out->cache_bytes = json_int(root, "cache_bytes");
    out->pid = (long)json_int(root, "pid");

    cJSON_Delete(root);
    return 0;
}