/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * config.h - where tbox keeps things and how it identifies to Telegram.
 *
 *   $TBOX_HOME (default ~/.tbox)
 *     config       api_id=... / api_hash=...  (written by `tbox login`)
 *     tdlib/       TDLib's database: the session, chats, message cache
 *
 * The API credentials come from https://my.telegram.org/apps. They can be
 * given as TBOX_API_ID / TBOX_API_HASH in the environment, which wins
 * over the config file.
 */
#ifndef TBOX_CONFIG_H
#define TBOX_CONFIG_H

#include <stddef.h>

typedef struct Config {
    char home[512];        /* ~/.tbox */
    char config_file[576]; /* ~/.tbox/config */
    char tdlib_dir[576];   /* ~/.tbox/tdlib */
    long api_id;
    char api_hash[128];
} Config;

/* Resolve the directories, create them, read config file and environment.
 * Missing credentials are not an error; see config_has_credentials. */
int config_load(Config *cfg, char *err, size_t errlen);

int config_has_credentials(const Config *cfg);

/* Parse "key=value" lines (comments with #) into cfg. Pure. */
void config_parse(Config *cfg, const char *text);

/* Write api_id/api_hash to the config file. */
int config_save(const Config *cfg, char *err, size_t errlen);

#endif
