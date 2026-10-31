/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox me - who am I logged in as.
 */
#include <stdio.h>

#include "tg/chat.h"
#include "cmd/commands.h"
#include "core/session.h"

int print_me(Td *td) {
    cJSON *me = td_request(td, jreq("getMe"), TD_TIMEOUT);
    if (td_is_error(me)) {
        fprintf(stderr, "error: getMe: %s\n", td_error_message(me));
        cJSON_Delete(me);
        return EXIT_ERROR;
    }
    char name[256];
    chat_user_name(me, name, sizeof name);
    printf("id:       %lld\n", jint(me, "id"));
    printf("name:     %s\n", name);
    if (chat_username(me)[0] != '\0') {
        printf("username: @%s\n", chat_username(me));
    }
    if (jstr(me, "phone_number")[0] != '\0') {
        printf("phone:    +%s\n", jstr(me, "phone_number"));
    }
    if (jbool(me, "is_premium")) {
        printf("premium:  yes\n");
    }
    cJSON_Delete(me);
    return EXIT_OK;
}

int cmd_me(int argc, char **argv) {
    (void)argv;
    if (argc != 0) {
        fprintf(stderr, "usage: tbox me\n");
        return EXIT_USAGE;
    }
    Session s;
    int rc = session_open(&s, 0);
    if (rc != EXIT_OK) {
        return rc;
    }
    rc = print_me(&s.td);
    session_close(&s);
    return rc;
}
