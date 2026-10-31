/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * config.h - where tbox keeps its session and how it identifies to Telegram.
 *
 *   $TBOX_HOME (default ~/.tbox)
 *     tdlib/     TDLib's database: the session, chats, message cache
 *
 * Telegram requires every client application to send an api_id and
 * api_hash (from https://my.telegram.org/apps). tbox reads them from the
 * environment, TBOX_API_ID and TBOX_API_HASH. A build can also carry a
 * default pair (cmake -DTBOX_API_ID=... -DTBOX_API_HASH=...), which the
 * environment overrides. Nothing is written to disk.
 */
#ifndef TBOX_CONFIG_H
#define TBOX_CONFIG_H

#include <stddef.h>

typedef struct Config {
    char home[512];        /* ~/.tbox */
    char tdlib_dir[576];   /* ~/.tbox/tdlib */
    long api_id;
    char api_hash[128];
} Config;

/* Resolve and create the directories, read the credentials.
 * Missing credentials are not an error here; see config_has_credentials. */
int config_load(Config *cfg, char *err, size_t errlen);

int config_has_credentials(const Config *cfg);

/* What to tell a user whose credentials are missing. */
extern const char *const CONFIG_CREDENTIALS_HELP;

#endif
