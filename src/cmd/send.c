/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox send <chat> <text...> - send a text message.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tg/chat.h"
#include "cmd/commands.h"
#include "tg/outbox.h"
#include "core/session.h"

/* argv[1..] joined with single spaces. Caller frees. */
static char *join_text(int argc, char **argv) {
    size_t len = 0;
    for (int i = 1; i < argc; i++) {
        len += strlen(argv[i]) + 1;
    }
    char *text = malloc(len + 1);
    if (text == NULL) {
        return NULL;
    }
    text[0] = '\0';
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            strcat(text, " ");
        }
        strcat(text, argv[i]);
    }
    return text;
}

int cmd_send(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: tbox send <chat> <text...>\n");
        return EXIT_USAGE;
    }
    char *text = join_text(argc, argv);
    if (text == NULL || text[0] == '\0') {
        free(text);
        fprintf(stderr, "error: empty message\n");
        return EXIT_USAGE;
    }

    Session s;
    int rc = session_open(&s, 0);
    if (rc != EXIT_OK) {
        free(text);
        return rc;
    }

    char err[1024];
    long long chat_id = chat_resolve(&s.td, argv[0], err, sizeof err);
    if (chat_id == 0) {
        fprintf(stderr, "error: %s\n", err);
        rc = EXIT_ERROR;
        goto out;
    }

    cJSON *content = jreq("inputMessageText");
    cJSON *formatted = jreq("formattedText");
    cJSON_AddStringToObject(formatted, "text", text);
    cJSON_AddItemToObject(content, "text", formatted);

    cJSON *req = jreq("sendMessage");
    cJSON_AddNumberToObject(req, "chat_id", (double)chat_id);
    cJSON_AddItemToObject(req, "input_message_content", content);

    cJSON *r = td_request(&s.td, req, TD_TIMEOUT);
    if (td_is_error(r)) {
        fprintf(stderr, "error: sendMessage: %s\n", td_error_message(r));
        cJSON_Delete(r);
        rc = EXIT_ERROR;
        goto out;
    }
    long long temp_id = jint(r, "id");
    cJSON_Delete(r);

    long long final_id = outbox_wait(&s.td, temp_id, 0, 60.0, err, sizeof err);
    if (final_id == 0) {
        fprintf(stderr, "error: %s\n", err);
        rc = EXIT_ERROR;
        goto out;
    }
    printf("sent to %lld (message %lld)\n", chat_id, final_id);

out:
    free(text);
    session_close(&s);
    return rc;
}
