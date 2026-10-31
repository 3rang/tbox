/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox upload <chat> <file> [caption...] - send a file as a document.
 * Use "me" as the chat to drop it into your Saved Messages.
 */
#include "cmd/commands.h"
#include "core/platform.h"
#include "core/session.h"
#include "tg/chat.h"
#include "tg/outbox.h"

int cmd_upload(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: tbox upload <chat> <file> [caption...]\n");
        return EXIT_USAGE;
    }

    /* TDLib reads the file itself and wants an absolute path. */
    char path[1024];
    FILE *probe = fopen(argv[1], "rb");
    if (probe == NULL || platform_absolute_path(argv[1], path, sizeof path) != 0) {
        fprintf(stderr, "error: cannot read file %s\n", argv[1]);
        return EXIT_ERROR;
    }
    fclose(probe);
    char caption[1024];
    join_args(argc, argv, 2, caption, sizeof caption);

    Session s;
    int rc = session_open(&s, 0);
    if (rc != EXIT_OK) {
        return rc;
    }

    char err[512];
    long long chat_id = chat_resolve(&s.td, argv[0], err, sizeof err);
    long long message_id = 0;
    if (chat_id != 0) {
        cJSON *file = jreq("inputFileLocal");
        cJSON_AddStringToObject(file, "path", path);
        cJSON *content = jreq("inputMessageDocument");
        cJSON_AddItemToObject(content, "document", file);
        if (caption[0] != '\0') {
            cJSON *formatted = jreq("formattedText");
            cJSON_AddStringToObject(formatted, "text", caption);
            cJSON_AddItemToObject(content, "caption", formatted);
        }
        message_id = outbox_send(&s.td, chat_id, content, err, sizeof err);
    }

    if (message_id != 0) {
        printf("uploaded %s to %lld (message %lld)\n", argv[1], chat_id, message_id);
    } else {
        fprintf(stderr, "error: %s\n", err);
        rc = EXIT_ERROR;
    }
    session_close(&s);
    return rc;
}
