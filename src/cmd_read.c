/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox read <chat> [-n COUNT] - print the last messages of a chat,
 * oldest first.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chat.h"
#include "commands.h"
#include "message.h"
#include "session.h"

#define MAX_MESSAGES 1000

/* Sender id -> display name, so each user/chat is fetched once. */
typedef struct NameCache {
    struct {
        long long id;
        char name[128];
    } entry[128];
    int n;
} NameCache;

static const char *sender_name(Td *td, NameCache *cache, long long user_id, long long chat_id) {
    long long key = user_id != 0 ? user_id : chat_id;
    for (int i = 0; i < cache->n; i++) {
        if (cache->entry[i].id == key) {
            return cache->entry[i].name;
        }
    }
    char name[128] = "?";
    cJSON *req;
    if (user_id != 0) {
        req = jreq("getUser");
        cJSON_AddNumberToObject(req, "user_id", (double)user_id);
    } else {
        req = jreq("getChat");
        cJSON_AddNumberToObject(req, "chat_id", (double)chat_id);
    }
    cJSON *r = td_request(td, req, TD_TIMEOUT);
    if (!td_is_error(r)) {
        if (user_id != 0) {
            chat_user_name(r, name, sizeof name);
        } else {
            snprintf(name, sizeof name, "%s", jstr(r, "title"));
        }
    }
    cJSON_Delete(r);

    if (cache->n == (int)(sizeof cache->entry / sizeof cache->entry[0])) {
        cache->n = 0; /* tiny cache: just start over */
    }
    int i = cache->n++;
    cache->entry[i].id = key;
    snprintf(cache->entry[i].name, sizeof cache->entry[i].name, "%s", name);
    return cache->entry[i].name;
}

/* Fetch up to `want` messages, newest first, into out[]. Returns count. */
static int fetch_history(Td *td, long long chat_id, int want, cJSON **out) {
    int have = 0;
    long long from_id = 0;

    /* TDLib answers from its local cache and may return fewer than asked
     * (even one) on the first call; keep paging until it returns nothing. */
    while (have < want) {
        cJSON *req = jreq("getChatHistory");
        cJSON_AddNumberToObject(req, "chat_id", (double)chat_id);
        cJSON_AddNumberToObject(req, "from_message_id", (double)from_id);
        cJSON_AddNumberToObject(req, "offset", 0);
        cJSON_AddNumberToObject(req, "limit", want - have < 100 ? want - have : 100);
        cJSON_AddBoolToObject(req, "only_local", 0);

        cJSON *r = td_request(td, req, TD_TIMEOUT);
        if (td_is_error(r)) {
            fprintf(stderr, "error: getChatHistory: %s\n", td_error_message(r));
            cJSON_Delete(r);
            return have > 0 ? have : -1;
        }
        cJSON *list = jobj(r, "messages");
        int n = cJSON_GetArraySize(list);
        if (n == 0) {
            cJSON_Delete(r);
            break;
        }
        for (int i = 0; i < n && have < want; i++) {
            cJSON *m = cJSON_DetachItemFromArray(list, 0);
            out[have++] = m;
            from_id = jint(m, "id");
        }
        cJSON_Delete(r);
    }
    return have;
}

int cmd_read(int argc, char **argv) {
    const char *chat = NULL;
    int count = 20;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            count = atoi(argv[++i]);
        } else if (chat == NULL) {
            chat = argv[i];
        } else {
            chat = NULL;
            break;
        }
    }
    if (chat == NULL || count <= 0 || count > MAX_MESSAGES) {
        fprintf(stderr, "usage: tbox read <chat> [-n COUNT]   (1..%d, default 20)\n", MAX_MESSAGES);
        return EXIT_USAGE;
    }

    Session s;
    int rc = session_open(&s, 0);
    if (rc != EXIT_OK) {
        return rc;
    }

    char err[1024];
    long long chat_id = chat_resolve(&s.td, chat, err, sizeof err);
    if (chat_id == 0) {
        fprintf(stderr, "error: %s\n", err);
        session_close(&s);
        return EXIT_ERROR;
    }

    cJSON **msgs = calloc((size_t)count, sizeof *msgs);
    int n = msgs != NULL ? fetch_history(&s.td, chat_id, count, msgs) : -1;
    if (n < 0) {
        free(msgs);
        session_close(&s);
        return EXIT_ERROR;
    }

    NameCache cache = { { { 0, "" } }, 0 };
    for (int i = n - 1; i >= 0; i--) {
        long long user_id, sender_chat;
        char when[32], text[4096];
        message_sender(msgs[i], &user_id, &sender_chat);
        message_time(jint(msgs[i], "date"), when, sizeof when);
        message_text(msgs[i], text, sizeof text);
        printf("[%s] %s: %s\n", when, sender_name(&s.td, &cache, user_id, sender_chat), text);
        cJSON_Delete(msgs[i]);
    }
    if (n == 0) {
        printf("(no messages)\n");
    }
    free(msgs);
    session_close(&s);
    return EXIT_OK;
}
