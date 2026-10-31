/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox login - sign in once by scanning a QR code with the Telegram app;
 * the session is kept in ~/.tbox/tdlib.
 *
 * Needs the API credentials of an application registered at
 * https://my.telegram.org/apps. They are asked for the first time and
 * saved to ~/.tbox/config (or taken from TBOX_API_ID / TBOX_API_HASH).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmd/commands.h"
#include "core/platform.h"
#include "core/session.h"
#include "tg/chat.h"

static int ask_credentials(Config *cfg) {
    char line[sizeof cfg->api_hash];
    printf("tbox needs a Telegram API id and hash. Create them at\n"
           "https://my.telegram.org/apps (any app name works).\n");
    if (platform_read_line("api_id: ", line, sizeof line) != 0) {
        return -1;
    }
    cfg->api_id = strtol(line, NULL, 10);
    if (platform_read_line("api_hash: ", line, sizeof line) != 0) {
        return -1;
    }
    snprintf(cfg->api_hash, sizeof cfg->api_hash, "%s", line);
    return config_has_credentials(cfg) ? 0 : -1;
}

int cmd_login(int argc, char **argv) {
    long api_id = 0;
    const char *api_hash = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--api-id") == 0 && i + 1 < argc) {
            api_id = strtol(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--api-hash") == 0 && i + 1 < argc) {
            api_hash = argv[++i];
        } else {
            fprintf(stderr, "usage: tbox login [--api-id ID --api-hash HASH]\n");
            return EXIT_USAGE;
        }
    }

    char err[512];
    Config cfg;
    if (config_load(&cfg, err, sizeof err) != 0) {
        fprintf(stderr, "error: %s\n", err);
        return EXIT_ERROR;
    }
    if (api_id > 0) {
        cfg.api_id = api_id;
    }
    if (api_hash != NULL) {
        snprintf(cfg.api_hash, sizeof cfg.api_hash, "%s", api_hash);
    }
    int save = api_id > 0 || api_hash != NULL;
    if (!config_has_credentials(&cfg)) {
        if (ask_credentials(&cfg) != 0) {
            fprintf(stderr, "error: api_id and api_hash are required\n");
            return EXIT_ERROR;
        }
        save = 1;
    }
    if (save && config_save(&cfg, err, sizeof err) != 0) {
        fprintf(stderr, "error: %s\n", err);
        return EXIT_ERROR;
    }

    Session s;
    int rc = session_open(&s, 1);
    if (rc != EXIT_OK) {
        return rc;
    }
    cJSON *me = td_request(&s.td, jreq("getMe"), TD_TIMEOUT);
    if (td_is_error(me)) {
        fprintf(stderr, "error: getMe: %s\n", td_error_message(me));
        rc = EXIT_ERROR;
    } else {
        char name[256];
        chat_user_name(me, name, sizeof name);
        printf("Logged in as %s", name);
        if (chat_username(me)[0] != '\0') {
            printf(" (@%s)", chat_username(me));
        }
        printf(", id %lld\n", jint(me, "id"));
    }
    cJSON_Delete(me);
    session_close(&s);
    return rc;
}
