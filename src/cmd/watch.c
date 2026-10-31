/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox watch [chat] - print every new incoming message as it arrives,
 * from all chats or from one. Each line shows the chat id, which is what
 * `tbox send` and `tbox upload` take. Ctrl-C stops it.
 */
#include <signal.h>

#include "cmd/commands.h"
#include "core/session.h"
#include "tg/chat.h"
#include "tg/message.h"

#define QUEUE_MAX 64

static volatile sig_atomic_t stop_requested;

static void on_sigint(int sig) {
    (void)sig;
    stop_requested = 1;
}

/* The update handler only queues; printing needs getChat/getUser
 * requests, which must not run from inside the handler. */
typedef struct Watch {
    long long only_chat;
    cJSON *queue[QUEUE_MAX];
    int n;
} Watch;

static void on_update(Td *td, const cJSON *ev, void *user) {
    (void)td;
    Watch *w = (Watch *)user;
    if (!jis(ev, "updateNewMessage")) {
        return;
    }
    const cJSON *msg = jobj(ev, "message");
    if (jbool(msg, "is_outgoing")) {
        return;
    }
    if (w->only_chat != 0 && jint(msg, "chat_id") != w->only_chat) {
        return;
    }
    if (w->n < QUEUE_MAX) {
        w->queue[w->n++] = cJSON_Duplicate(msg, 1);
    }
}

static void print_message(Td *td, const cJSON *msg) {
    long long chat_id = jint(msg, "chat_id");
    long long user_id, sender_chat;
    char when[32], title[128], sender[128], text[4096];

    message_time(jint(msg, "date"), when, sizeof when);
    chat_title(td, chat_id, title, sizeof title);
    message_sender(msg, &user_id, &sender_chat);
    message_text(msg, text, sizeof text);

    /* In a private chat the sender is the chat; say it once. */
    if (user_id != 0 && user_id != chat_id) {
        chat_user_display(td, user_id, sender, sizeof sender);
        printf("[%s] %s (%lld) %s: %s\n", when, title, chat_id, sender, text);
    } else {
        printf("[%s] %s (%lld): %s\n", when, title, chat_id, text);
    }
    fflush(stdout);
}

int cmd_watch(int argc, char **argv) {
    if (argc > 1) {
        fprintf(stderr, "usage: tbox watch [chat]\n");
        return EXIT_USAGE;
    }
    Session s;
    int rc = session_open(&s, 0);
    if (rc != EXIT_OK) {
        return rc;
    }

    Watch w;
    memset(&w, 0, sizeof w);
    if (argc == 1) {
        char err[1024];
        w.only_chat = chat_resolve(&s.td, argv[0], err, sizeof err);
        if (w.only_chat == 0) {
            fprintf(stderr, "error: %s\n", err);
            session_close(&s);
            return EXIT_ERROR;
        }
    }

    signal(SIGINT, on_sigint);
#ifdef SIGTERM
    signal(SIGTERM, on_sigint);
#endif
    fprintf(stderr, "watching%s... press Ctrl-C to stop\n", argc == 1 ? " this chat" : "");

    td_set_update_handler(&s.td, on_update, &w);
    while (!stop_requested && !s.td.closed) {
        td_pump(&s.td, 0.5);
        /* Drain outside the handler: printing talks to TDLib too. */
        for (int i = 0; i < w.n; i++) {
            print_message(&s.td, w.queue[i]);
            cJSON_Delete(w.queue[i]);
        }
        w.n = 0;
    }
    td_set_update_handler(&s.td, NULL, NULL);

    session_close(&s);
    return EXIT_OK;
}
