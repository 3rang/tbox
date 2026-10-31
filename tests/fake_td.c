/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 */
#include "fake_td.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_EVENTS 256
#define MAX_RULES 64
#define MAX_SENT 256

typedef struct Rule {
    char type[64];
    char *reply;
    char *update;   /* may be NULL */
    int remaining;  /* -1 = forever */
} Rule;

static struct {
    char *events[MAX_EVENTS];
    int head, tail;
    char *idle_events[MAX_EVENTS];
    int idle_head, idle_tail;
    Rule rules[MAX_RULES];
    int nrules;
    cJSON *sent[MAX_SENT];
    int nsent;
    char *last_received;
    char last_execute[256];
} F;

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d != NULL) {
        memcpy(d, s, n);
    }
    return d;
}

void fake_td_reset(void) {
    for (int i = F.head; i < F.tail; i++) {
        free(F.events[i]);
    }
    for (int i = F.idle_head; i < F.idle_tail; i++) {
        free(F.idle_events[i]);
    }
    for (int i = 0; i < F.nrules; i++) {
        free(F.rules[i].reply);
        free(F.rules[i].update);
    }
    for (int i = 0; i < F.nsent; i++) {
        cJSON_Delete(F.sent[i]);
    }
    free(F.last_received);
    memset(&F, 0, sizeof F);
}

void fake_td_push(const char *json) {
    if (F.tail < MAX_EVENTS) {
        F.events[F.tail++] = dup_str(json);
    }
}

void fake_td_push_when_idle(const char *json) {
    if (F.idle_tail < MAX_EVENTS) {
        F.idle_events[F.idle_tail++] = dup_str(json);
    }
}

static void add_rule(const char *type, const char *reply, const char *update, int remaining) {
    if (F.nrules == MAX_RULES) {
        return;
    }
    Rule *r = &F.rules[F.nrules++];
    snprintf(r->type, sizeof r->type, "%s", type);
    r->reply = dup_str(reply);
    r->update = update != NULL ? dup_str(update) : NULL;
    r->remaining = remaining;
}

void fake_td_on(const char *type, const char *reply_json) {
    add_rule(type, reply_json, NULL, -1);
}

void fake_td_once(const char *type, const char *reply_json, const char *update_json) {
    add_rule(type, reply_json, update_json, 1);
}

/* --- the four td_json_client functions ---------------------------------- */

static int fake_create_client_id(void) {
    return 1;
}

static void fake_send(int client_id, const char *request) {
    (void)client_id;
    cJSON *req = cJSON_Parse(request);
    if (req == NULL) {
        return;
    }
    if (F.nsent < MAX_SENT) {
        F.sent[F.nsent++] = req;
    }
    const char *type = jtype(req);
    for (int i = 0; i < F.nrules; i++) {
        Rule *r = &F.rules[i];
        if (r->remaining == 0 || strcmp(r->type, type) != 0) {
            continue;
        }
        if (r->remaining > 0) {
            r->remaining--;
        }
        if (r->update != NULL && r->update[0] == '[') {
            /* A JSON array: several updates, in order. */
            cJSON *arr = cJSON_Parse(r->update);
            cJSON *u;
            cJSON_ArrayForEach(u, arr) {
                char *text = cJSON_PrintUnformatted(u);
                fake_td_push(text);
                free(text);
            }
            cJSON_Delete(arr);
        } else if (r->update != NULL) {
            fake_td_push(r->update);
        }
        cJSON *reply = cJSON_Parse(r->reply);
        if (reply != NULL) {
            const char *extra = jstr(req, "@extra");
            if (extra[0] != '\0') {
                cJSON_AddStringToObject(reply, "@extra", extra);
            }
            char *text = cJSON_PrintUnformatted(reply);
            fake_td_push(text);
            free(text);
            cJSON_Delete(reply);
        }
        return;
    }
    /* No rule: the request stays unanswered, like a dropped connection. */
}

static const char *fake_receive(double timeout) {
    (void)timeout;
    if (F.head == F.tail) {
        /* Nothing pending: the program is waiting. Now "the phone" acts. */
        if (F.idle_head == F.idle_tail) {
            return NULL;
        }
        free(F.last_received);
        F.last_received = F.idle_events[F.idle_head];
        F.idle_events[F.idle_head++] = NULL;
        return F.last_received;
    }
    free(F.last_received);
    F.last_received = F.events[F.head];
    F.events[F.head++] = NULL;
    return F.last_received;
}

static const char *fake_execute(const char *request) {
    snprintf(F.last_execute, sizeof F.last_execute, "%s", request);
    return "{\"@type\":\"ok\"}";
}

const TdApi *fake_td_api(void) {
    static const TdApi api = { fake_create_client_id, fake_send, fake_receive, fake_execute };
    return &api;
}

/* --- inspection --------------------------------------------------------- */

int fake_td_sent_count(void) {
    return F.nsent;
}

cJSON *fake_td_sent(int i) {
    return i >= 0 && i < F.nsent ? F.sent[i] : NULL;
}

cJSON *fake_td_last_of(const char *type) {
    for (int i = F.nsent - 1; i >= 0; i--) {
        if (jis(F.sent[i], type)) {
            return F.sent[i];
        }
    }
    return NULL;
}

int fake_td_count_of(const char *type) {
    int n = 0;
    for (int i = 0; i < F.nsent; i++) {
        n += jis(F.sent[i], type);
    }
    return n;
}

const char *fake_td_last_execute(void) {
    return F.last_execute;
}
