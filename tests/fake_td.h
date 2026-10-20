/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * fake_td.h — scripted tdjson double for client tests (header-only).
 *
 * The fake records every send() and replays queued events through
 * receive(). Strings it returns stay valid for the whole test; free them
 * with fake_td_free().
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

#define FAKE_MAX 16

typedef struct FakeTd {
    TBoxTdApi api;
    int client_id;
    int verbosity;
    char *sent[FAKE_MAX];
    int sent_n;
    char *queue[FAKE_MAX];
    int qn;
    int qhead; /* events replay FIFO, like td_receive does */
} FakeTd;

/* The fake needs one file-scope instance because TBoxTdApi callbacks
 * carry no user-data pointer (they mirror the real C signatures). */
static FakeTd *fake_g;

static int fake_create_client_id(void) { return fake_g->client_id; }
static void fake_set_verbosity(int v) { fake_g->verbosity = v; }

static void fake_send(int client_id, const char *request) {
    (void)client_id;
    if (fake_g->sent_n < FAKE_MAX) {
        fake_g->sent[fake_g->sent_n++] = _strdup(request);
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
    if (f->qn < FAKE_MAX) {
        f->queue[f->qn++] = _strdup(event_json);
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
