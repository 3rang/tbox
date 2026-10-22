/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox upload <chat> <file> [caption...] - send a file as a document.
 * Use "me" as the chat to drop it into your Saved Messages.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chat.h"
#include "commands.h"
#include "outbox.h"
#include "platform.h"
#include "session.h"

static void join_caption(int argc, char **argv, char *buf, size_t n) {
    buf[0] = '\0';
    for (int i = 2; i < argc; i++) {
        size_t used = strlen(buf);
        snprintf(buf + used, n - used, "%s%s", i > 2 ? " " : "", argv[i]);
    }
}

int cmd_upload(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: tbox upload <chat> <file> [caption...]\n");
        return EXIT_USAGE;
    }

    char path[1024];
    if (!platform_file_exists(argv[1]) || platform_absolute_path(argv[1], path, sizeof path) != 0) {
        fprintf(stderr, "error: cannot read file %s\n", argv[1]);
        return EXIT_ERROR;
    }
    char caption[1024];
    join_caption(argc, argv, caption, sizeof caption);

    Session s;
    int rc = session_open(&s, 0);
    if (rc != EXIT_OK) {
        return rc;
    }

    char err[1024];
    long long chat_id = chat_resolve(&s.td, argv[0], err, sizeof err);
    if (chat_id == 0) {
        fprintf(stderr, "error: %s\n", err);
        rc = EXIT_ERROR;
        goto out;
    }

    /* inputFileLocal: TDLib reads and uploads the file itself. */
    cJSON *file = jreq("inputFileLocal");
    cJSON_AddStringToObject(file, "path", path);

    cJSON *content = jreq("inputMessageDocument");
    cJSON_AddItemToObject(content, "document", file);
    if (caption[0] != '\0') {
        cJSON *formatted = jreq("formattedText");
        cJSON_AddStringToObject(formatted, "text", caption);
        cJSON_AddItemToObject(content, "caption", formatted);
    }

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
    long long file_id = jint(jobj(jobj(jobj(r, "content"), "document"), "document"), "id");
    cJSON_Delete(r);

    /* Large files take as long as they take; only silence times out. */
    long long final_id = outbox_wait(&s.td, temp_id, file_id, 120.0, err, sizeof err);
    if (final_id == 0) {
        fprintf(stderr, "error: %s\n", err);
        rc = EXIT_ERROR;
        goto out;
    }
    printf("uploaded %s to %lld (message %lld)\n", argv[1], chat_id, final_id);

out:
    session_close(&s);
    return rc;
}
