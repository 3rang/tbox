/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * fake_td.h — scripted tdjson double for client tests (header-only).
 *
 * The fake records every send() and replays queued events through
 * receive(). Strings it returns stay valid for the whole test; free them
 * with fake_td_free().
 *
 * A responder can be installed to make the fake answer requests the way
 * TDLib does: the callback sees the request and returns a reply (or NULL for
 * no answer). The fake then puts the request's @extra on that reply, so a
 * caller that pairs requests and replies by @extra - which is what
 * tbox_session_call() does - finds its answer.
 */
#ifndef FAKE_TD_H
#define FAKE_TD_H

#include "td/tbox_td_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "td/tbox_td_client.h"

#ifdef _WIN32
#include <windows.h>
#define FAKE_SLEEP_MS(ms) Sleep(ms)
#else
#include <unistd.h>
#define FAKE_SLEEP_MS(ms) usleep((ms) * 1000)
#endif

#define FAKE_MAX 16      /* sent requests recorded (scripted flows are short) */
#define FAKE_QUEUE_MAX 65536 /* a fake must be able to stage a real burst */

/* Answers one request; returns a reply JSON the fake frees, or NULL. */
typedef char *(*FakeTdResponderFn)(void *ctx, const char *request);

typedef struct FakeTd {
    TBoxTdApi api;
    int client_id;
    int verbosity;
    char *sent[FAKE_MAX];
    int sent_n;
    char *queue[FAKE_QUEUE_MAX];
    int qn;
    int qhead; /* events replay FIFO, like td_receive does */
    FakeTdResponderFn responder;
    void *responder_ctx;
} FakeTd;

/* The fake needs one file-scope instance because TBoxTdApi callbacks
 * carry no user-data pointer (they mirror the real C signatures). */
static FakeTd *fake_g;

static void fake_td_enqueue(FakeTd *f, const char *event_json);
static void fake_td_answer(FakeTd *f, const char *request,
                           const char *reply);

static int fake_create_client_id(void) { return fake_g->client_id; }
static void fake_set_verbosity(int v) { fake_g->verbosity = v; }

static void fake_send(int client_id, const char *request) {
    (void)client_id;
    if (fake_g->sent_n < FAKE_MAX) {
        fake_g->sent[fake_g->sent_n++] = _strdup(request);
    }
    if (fake_g->responder != NULL) {
        char *reply = fake_g->responder(fake_g->responder_ctx, request);
        if (reply != NULL) {
            fake_td_answer(fake_g, request, reply);
            free(reply);
        }
    }
}

static const char *fake_receive(double timeout) {
    if (fake_g->qhead >= fake_g->qn) {
        FAKE_SLEEP_MS(timeout < 0.05 ? (int)(timeout * 1000.0) : 50);
        return NULL;
    }
    return fake_g->queue[fake_g->qhead++];
}

static FakeTd *fake_td_start(int client_id) {
    static FakeTd g;
    fake_g = &g;
    memset(&g, 0, sizeof g);
    g.api.set_verbosity = fake_set_verbosity;
    g.api.create_client_id = fake_create_client_id;
    g.api.send = fake_send;
    g.api.receive = fake_receive;
    g.client_id = client_id;
    return &g;
}

static void fake_td_enqueue(FakeTd *f, const char *event_json) {
    if (f->qn < FAKE_QUEUE_MAX) {
        f->queue[f->qn++] = _strdup(event_json);
    }
}

/* Puts the request's @extra on the reply and queues it, so the caller pairs
 * request and reply the way TDLib does. */
static void fake_td_answer(FakeTd *f, const char *request,
                           const char *reply) {
    cJSON *req = cJSON_Parse(request);
    cJSON *rep = cJSON_Parse(reply);
    if (req != NULL && rep != NULL) {
        const cJSON *extra = cJSON_GetObjectItemCaseSensitive(req, "@extra");
        if (cJSON_IsString(extra)) {
            cJSON_DeleteItemFromObjectCaseSensitive(rep, "@extra");
            cJSON_AddStringToObject(rep, "@extra",
                                    cJSON_GetStringValue(extra));
        }
    }
    char *out = rep != NULL ? cJSON_PrintUnformatted(rep) : NULL;
    cJSON_Delete(req);
    cJSON_Delete(rep);
    if (out != NULL) {
        fake_td_enqueue(f, out);
        free(out);
    } else {
        fake_td_enqueue(f, reply);
    }
}

static void fake_td_free(FakeTd *f) {
    for (int i = 0; i < f->sent_n; i++) {
        free(f->sent[i]);
    }
    for (int i = 0; i < f->qn; i++) {
        free(f->queue[i]);
    }
}

#endif /* FAKE_TD_H */
