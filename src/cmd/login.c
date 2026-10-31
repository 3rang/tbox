/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox login - sign in once by scanning a QR code with the Telegram app;
 * the session is kept in ~/.tbox/tdlib.
 */
#include <stdio.h>

#include "cmd/commands.h"
#include "core/session.h"
#include "tg/chat.h"

int cmd_login(int argc, char **argv) {
    (void)argv;
    if (argc != 0) {
        fprintf(stderr, "usage: tbox login\n");
        return EXIT_USAGE;
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
