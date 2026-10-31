/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox send <chat> <text...> - send a text message.
 */
#include "cmd/commands.h"
#include "core/session.h"
#include "tg/chat.h"
#include "tg/outbox.h"

int cmd_send(int argc, char **argv) {
    char text[4096]; /* Telegram's limit for one message */
    if (argc >= 2) {
        join_args(argc, argv, 1, text, sizeof text);
    }
    if (argc < 2 || text[0] == '\0') {
        fprintf(stderr, "usage: tbox send <chat> <text...>\n");
        return EXIT_USAGE;
    }

    Session s;
    int rc = session_open(&s, 0);
    if (rc != EXIT_OK) {
        return rc;
    }

    char err[512];
    long long chat_id = chat_resolve(&s.td, argv[0], err, sizeof err);
    long long message_id = 0;
    if (chat_id != 0) {
        cJSON *content = jreq("inputMessageText");
        cJSON *formatted = jreq("formattedText");
        cJSON_AddStringToObject(formatted, "text", text);
        cJSON_AddItemToObject(content, "text", formatted);
        message_id = outbox_send(&s.td, chat_id, content, err, sizeof err);
    }

    if (message_id != 0) {
        printf("sent to %lld (message %lld)\n", chat_id, message_id);
    } else {
        fprintf(stderr, "error: %s\n", err);
        rc = EXIT_ERROR;
    }
    session_close(&s);
    return rc;
}
