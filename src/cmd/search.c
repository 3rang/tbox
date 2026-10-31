/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox search <query> - find chats by title (the ones you are in) and by
 * public @username, and print their ids for use with send/read/upload.
 */
#include <stdio.h>
#include <string.h>

#include "tg/chat.h"
#include "cmd/commands.h"
#include "core/session.h"

#define MAX_RESULTS 64

typedef struct Seen {
    long long ids[MAX_RESULTS];
    int n;
} Seen;

static int seen_add(Seen *s, long long id) {
    for (int i = 0; i < s->n; i++) {
        if (s->ids[i] == id) {
            return 0;
        }
    }
    if (s->n < MAX_RESULTS) {
        s->ids[s->n++] = id;
    }
    return 1;
}

/* Print every chat in a `chats` reply not printed before. */
static void print_chats(Td *td, cJSON *reply, Seen *seen, const char *what) {
    if (td_is_error(reply)) {
        fprintf(stderr, "warning: %s: %s\n", what, td_error_message(reply));
        cJSON_Delete(reply);
        return;
    }
    cJSON *ids = jobj(reply, "chat_ids");
    cJSON *id;
    cJSON_ArrayForEach(id, ids) {
        long long chat_id = (long long)id->valuedouble;
        if (!seen_add(seen, chat_id)) {
            continue;
        }
        cJSON *req = jreq("getChat");
        cJSON_AddNumberToObject(req, "chat_id", (double)chat_id);
        cJSON *chat = td_request(td, req, TD_TIMEOUT);
        if (!td_is_error(chat)) {
            char line[512];
            chat_describe(td, chat, line, sizeof line);
            printf("%s\n", line);
        }
        cJSON_Delete(chat);
    }
    cJSON_Delete(reply);
}

int cmd_search(int argc, char **argv) {
    if (argc != 1) {
        fprintf(stderr, "usage: tbox search <query>\n");
        return EXIT_USAGE;
    }
    Session s;
    int rc = session_open(&s, 0);
    if (rc != EXIT_OK) {
        return rc;
    }

    Seen seen = { { 0 }, 0 };

    /* Chats in your list, by title (locally known ones first, then the
     * server's view), then public usernames anyone can find. */
    cJSON *req = jreq("searchChats");
    cJSON_AddStringToObject(req, "query", argv[0]);
    cJSON_AddNumberToObject(req, "limit", 20);
    print_chats(&s.td, td_request(&s.td, req, TD_TIMEOUT), &seen, "searchChats");

    req = jreq("searchChatsOnServer");
    cJSON_AddStringToObject(req, "query", argv[0]);
    cJSON_AddNumberToObject(req, "limit", 20);
    print_chats(&s.td, td_request(&s.td, req, TD_TIMEOUT), &seen, "searchChatsOnServer");

    if (strlen(argv[0]) >= 4) { /* Telegram rejects shorter public queries */
        req = jreq("searchPublicChats");
        cJSON_AddStringToObject(req, "query", argv[0]);
        print_chats(&s.td, td_request(&s.td, req, TD_TIMEOUT), &seen, "searchPublicChats");
    }

    if (seen.n == 0) {
        printf("no chats match \"%s\"\n", argv[0]);
    }
    session_close(&s);
    return EXIT_OK;
}
