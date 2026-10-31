/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 */
#include "core/config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/platform.h"

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return s;
}

void config_parse(Config *cfg, const char *text) {
    while (*text != '\0') {
        const char *nl = strchr(text, '\n');
        size_t len = nl != NULL ? (size_t)(nl - text) : strlen(text);
        char line[512];
        if (len >= sizeof line) {
            len = sizeof line - 1;
        }
        memcpy(line, text, len);
        line[len] = '\0';
        text += nl != NULL ? len + 1 : len;

        char *hash = strchr(line, '#');
        if (hash != NULL) {
            *hash = '\0';
        }
        char *eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        const char *key = trim(line);
        const char *val = trim(eq + 1);
        if (strcmp(key, "api_id") == 0) {
            cfg->api_id = strtol(val, NULL, 10);
        } else if (strcmp(key, "api_hash") == 0) {
            snprintf(cfg->api_hash, sizeof cfg->api_hash, "%s", val);
        }
    }
}

static int read_file(const char *path, char *buf, size_t n) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    size_t got = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[got] = '\0';
    return 0;
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
    snprintf(cfg->config_file, sizeof cfg->config_file, "%s/config", cfg->home);
    snprintf(cfg->tdlib_dir, sizeof cfg->tdlib_dir, "%s/tdlib", cfg->home);

    if (platform_mkdir(cfg->home) != 0 || platform_mkdir(cfg->tdlib_dir) != 0) {
        snprintf(err, errlen, "cannot create %s", cfg->tdlib_dir);
        return -1;
    }

    char text[4096];
    if (read_file(cfg->config_file, text, sizeof text) == 0) {
        config_parse(cfg, text);
    }

    const char *id = getenv("TBOX_API_ID");
    const char *hash = getenv("TBOX_API_HASH");
    if (id != NULL && id[0] != '\0') {
        cfg->api_id = strtol(id, NULL, 10);
    }
    if (hash != NULL && hash[0] != '\0') {
        snprintf(cfg->api_hash, sizeof cfg->api_hash, "%s", hash);
    }
    return 0;
}

int config_has_credentials(const Config *cfg) {
    return cfg->api_id > 0 && cfg->api_hash[0] != '\0';
}

int config_save(const Config *cfg, char *err, size_t errlen) {
    FILE *f = fopen(cfg->config_file, "w");
    if (f == NULL) {
        snprintf(err, errlen, "cannot write %s", cfg->config_file);
        return -1;
    }
    fprintf(f, "# tbox - Telegram API credentials from https://my.telegram.org/apps\n");
    fprintf(f, "api_id=%ld\n", cfg->api_id);
    fprintf(f, "api_hash=%s\n", cfg->api_hash);
    fclose(f);
    return 0;
}
