/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 */
#include "core/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/platform.h"

#ifndef TBOX_DEFAULT_API_ID
#define TBOX_DEFAULT_API_ID ""
#endif
#ifndef TBOX_DEFAULT_API_HASH
#define TBOX_DEFAULT_API_HASH ""
#endif

const char *const CONFIG_CREDENTIALS_HELP =
    "tbox needs Telegram API credentials. Get them at https://my.telegram.org/apps\n"
    "(log in, create an application, any name), then put them in the environment:\n"
    "\n"
    "    export TBOX_API_ID=12345\n"
    "    export TBOX_API_HASH=0123456789abcdef0123456789abcdef\n"
    "\n"
    "Add those two lines to your shell profile so every tbox run sees them.";

static const char *env_or(const char *name, const char *fallback) {
    const char *v = getenv(name);
    return v != NULL && v[0] != '\0' ? v : fallback;
}

int config_load(Config *cfg, char *err, size_t errlen) {
    memset(cfg, 0, sizeof *cfg);

    const char *home = getenv("TBOX_HOME");
    if (home != NULL && home[0] != '\0') {
        snprintf(cfg->home, sizeof cfg->home, "%s", home);
    } else {
        const char *user_home = platform_home_dir();
        if (user_home == NULL) {
            snprintf(err, errlen, "cannot find the home directory; set TBOX_HOME");
            return -1;
        }
        snprintf(cfg->home, sizeof cfg->home, "%s/.tbox", user_home);
    }
    snprintf(cfg->tdlib_dir, sizeof cfg->tdlib_dir, "%s/tdlib", cfg->home);

    if (platform_mkdir(cfg->home) != 0 || platform_mkdir(cfg->tdlib_dir) != 0) {
        snprintf(err, errlen, "cannot create %s", cfg->tdlib_dir);
        return -1;
    }

    cfg->api_id = strtol(env_or("TBOX_API_ID", TBOX_DEFAULT_API_ID), NULL, 10);
    snprintf(cfg->api_hash, sizeof cfg->api_hash, "%s", env_or("TBOX_API_HASH", TBOX_DEFAULT_API_HASH));
    return 0;
}

int config_has_credentials(const Config *cfg) {
    return cfg->api_id > 0 && cfg->api_hash[0] != '\0';
}
