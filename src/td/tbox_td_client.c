/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox_td_client.c — the receive loop.
 *
 * Exactly ONE thread calls api->receive: updates keep FIFO order and the
 * "valid until next call" rule of tdjson stays safe. td_send may be called
 * from any thread. No tdjson symbols appear here — the TBoxTdApi table is
 * injected (real one from tbox_td_api.c, fakes from tests).
 */
#include "td/tbox_td_client.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#include <stdatomic.h>
#endif

struct TBoxTdClient {
    const TBoxTdApi *api;
    int client_id;
    TBoxTdUpdateFn handler;
    void *user_data;
#ifdef _WIN32
    volatile LONG closed;
    HANDLE thread;
#else
    atomic_bool closed;
    pthread_t thread;
#endif
};

static void dispatch(TBoxTdClient *c, const char *raw) {
    cJSON *root = cJSON_Parse(raw);
    if (root == NULL) {
        return; /* malformed event: skip, never crash the loop */
    }
    if (cJSON_GetObjectItemCaseSensitive(root, "@extra") == NULL) {
        if (c->handler != NULL) {
            c->handler(c->user_data, root);
        }
    }
    /* events carrying @extra are replies to fire-and-forget requests:
     * nobody waits for them here, so they are dropped */
    cJSON_Delete(root);
}

#ifdef _WIN32
static unsigned __stdcall recv_loop(void *arg) {
    TBoxTdClient *c = arg;
    while (InterlockedExchangeAdd(&c->closed, 0) == 0) {
        const char *raw = c->api->receive(1.0);
        if (raw != NULL) {
            dispatch(c, raw);
        }
    }
    return 0;
}
#else
static void *recv_loop(void *arg) {
    TBoxTdClient *c = arg;
    while (!atomic_load(&c->closed)) {
        const char *raw = c->api->receive(1.0);
        if (raw != NULL) {
            dispatch(c, raw);
        }
    }
    return NULL;
}
#endif

TBoxTdClient *tbox_td_client_open(const TBoxTdApi *api, int verbosity) {
    if (api == NULL) {
        return NULL;
    }
    TBoxTdClient *c = calloc(1, sizeof(TBoxTdClient));
    if (c == NULL) {
        return NULL;
    }
    c->api = api;
    api->set_verbosity(verbosity);
    c->client_id = api->create_client_id();
#ifdef _WIN32
    c->thread = (HANDLE)_beginthreadex(NULL, 0, recv_loop, c, 0, NULL);
    if (c->thread == NULL) {
        free(c);
        return NULL;
    }
#else
    atomic_init(&c->closed, false);
    if (pthread_create(&c->thread, NULL, recv_loop, c) != 0) {
        free(c);
        return NULL;
    }
#endif
    return c;
}

void tbox_td_client_on_update(TBoxTdClient *c, TBoxTdUpdateFn handler, void *user_data) {
    if (c != NULL) {
        c->handler = handler;
        c->user_data = user_data;
    }
}

void tbox_td_client_send(TBoxTdClient *c, const char *request_json) {
    if (c != NULL && request_json != NULL) {
        c->api->send(c->client_id, request_json);
    }
}

void tbox_td_client_close(TBoxTdClient *c) {
    if (c == NULL) {
        return;
    }
#ifdef _WIN32
    InterlockedExchange(&c->closed, 1);
    WaitForSingleObject(c->thread, 3000);
    CloseHandle(c->thread);
#else
    atomic_store(&c->closed, true);
    pthread_join(c->thread, NULL);
#endif
    free(c);
}
