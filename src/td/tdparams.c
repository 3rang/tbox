/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * tdparams.c - TDLib client configuration: credentials, per-OS data
 * directory, and the setTdlibParameters request.
 */

#define _CRT_SECURE_NO_WARNINGS   /* getenv/snprintf on MSVC */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L   /* mkdir visible under -std=c17 */
#endif

#include "tdparams.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/datadir.h"

/* Credentials are read from the environment only - there is no config
 * file and nothing compiled in, so the same binary works wherever the
 * two variables are exported:
 *   Windows  : set TG_API_ID=<id>; set TG_API_HASH=<hash>
 *   Linux    : export TG_API_ID=<id> TG_API_HASH=<hash>
 * Both are required; a bad value is reported (see load_credentials). */

#ifndef TBOX_VERSION
#define TBOX_VERSION "0.5.0"
#endif

#ifndef TBOX_PATH_MAX
#define TBOX_PATH_MAX 512
#endif



/* The per-OS data root and the TDLib session subdir now live in
 * util/datadir.c, so `tbox serve` / `tbox status` (which never link
 * TDLib) resolve exactly the same paths as the login flow. The two wrappers
 * below keep the td/ API stable. */

int tbox_td_default_data_dir(char *out, size_t size)
{
    return tbox_datadir_default(out, size);
}

int tbox_td_prepare_dir(tbox_td_params_t *params)
{
    char root[TBOX_PATH_MAX];
    size_t len;

    if (params == NULL)
        return -1;

    if (params->data_dir[0] == '\0') {
        if (tbox_datadir_default(root, sizeof root) != 0)
            return -1;
    } else {
        len = strlen(params->data_dir);
        if (len == 0 || len >= sizeof root)
            return -1;
        memcpy(root, params->data_dir, len + 1);
    }

    /* TDLib owns its database files in a dedicated subdir; keeping it
     * separate lets tbox add caches/ledger files to `root` later. */
    if (tbox_datadir_session(params->data_dir, sizeof params->data_dir, root) != 0)
        return -1;

    (void)tbox_datadir_ensure(root);
    (void)tbox_datadir_ensure(params->data_dir);

    return 0;
}

/* api_hash: exactly 32 hex digits (Telegram issues lowercase hex). */
static int valid_api_hash(const char *s)
{
    int n = 0;

    if (s == NULL)
        return 0;
    for (; *s != '\0'; s++, n++) {
        int c = (unsigned char)*s;
        if (!isxdigit(c))
            return 0;
    }
    return n == 32;
}

int tbox_td_load_credentials(tbox_td_params_t *params)
{
    const char *id_env;
    const char *hash_env;
    long parsed;
    char *end;

    if (params == NULL)
        return -1;

    id_env = getenv("TG_API_ID");
    hash_env = getenv("TG_API_HASH");

    /* api_id: digits only, positive, fits an int */
    if (id_env == NULL || id_env[0] == '\0') {
        fprintf(stderr,
                "tbox auth: TG_API_ID is not set (https://my.telegram.org/apps).\n"
#ifdef _WIN32
                "  set TG_API_ID=<id> && set TG_API_HASH=<hash>\n"
#else
                "  export TG_API_ID=<id> TG_API_HASH=<hash>\n"
#endif
                );
        return -1;
    }
    parsed = strtol(id_env, &end, 10);
    if (id_env[0] < '0' || id_env[0] > '9' || *end != '\0') {
        fprintf(stderr, "tbox auth: TG_API_ID must be digits only.\n");
        return -1;
    }
    if (parsed <= 0 || parsed > 2147483647L) {
        fprintf(stderr, "tbox auth: TG_API_ID out of range.\n");
        return -1;
    }
    params->api_id = (int)parsed;

    /* api_hash: exactly 32 hex digits. The value itself is a credential,
     * so only its length is echoed when it is malformed. */
    if (hash_env == NULL || hash_env[0] == '\0') {
        fprintf(stderr, "tbox auth: TG_API_HASH is not set.\n");
        return -1;
    }
    if (!valid_api_hash(hash_env)) {
        fprintf(stderr,
                "tbox auth: TG_API_HASH must be 32 hex digits (got %u).\n",
                (unsigned)strlen(hash_env));
        return -1;
    }
    /* our own copy: the environment block may move under us */
    snprintf(params->api_hash, sizeof params->api_hash, "%s", hash_env);

    return 0;
}

cJSON *tbox_td_build_parameters(const tbox_td_params_t *params)
{
    cJSON *p;

    if (params == NULL)
        return NULL;

    p = cJSON_CreateObject();
    if (p == NULL)
        return NULL;

    cJSON_AddStringToObject(p, "@type", "setTdlibParameters");
    cJSON_AddNumberToObject(p, "api_id", (double)params->api_id);
    cJSON_AddStringToObject(p, "api_hash", params->api_hash);
    cJSON_AddBoolToObject(p, "use_test_dc", 0);
    cJSON_AddStringToObject(p, "database_directory", params->data_dir);
    cJSON_AddBoolToObject(p, "use_file_database", 1);
    cJSON_AddBoolToObject(p, "use_chat_info_database", 1);
    cJSON_AddBoolToObject(p, "use_message_database", 1);
    cJSON_AddBoolToObject(p, "use_secret_chats", 0);
    cJSON_AddStringToObject(p, "system_language_code", "en");
    cJSON_AddStringToObject(p, "device_model", "tbox");
    cJSON_AddStringToObject(p, "application_version", TBOX_VERSION);

    return p;
}
