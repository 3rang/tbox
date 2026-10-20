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
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define TBOX_STRDUP _strdup
#else
#include <pthread.h>
#include <stdatomic.h>
#define TBOX_STRDUP strdup
#endif

struct TBoxTdClient {
    const TBoxTdApi *api;
    int client_id;
    TBoxTdUpdateFn handler;
    void *user_data;
    /* replies (events with @extra): small ring, guarded by lock */
    char *replies[4];
    int rhead; /* oldest */
    int rtail; /* next write */
    int rcount;
    int started;
#ifdef _WIN32
    volatile LONG closed;
    HANDLE thread;
    CRITICAL_SECTION lock;
#else
    atomic_bool closed;
    pthread_t thread;
    pthread_mutex_t lock;
#endif
};

static void lock_client(TBoxTdClient *c) {
#ifdef _WIN32
    EnterCriticalSection(&c->lock);
#else
    pthread_mutex_lock(&c->lock);
#endif
}

static void unlock_client(TBoxTdClient *c) {
#ifdef _WIN32
    LeaveCriticalSection(&c->lock);
#else
    pthread_mutex_unlock(&c->lock);
#endif
}

/* Queue a reply copy; drops only when 4 unread pile up (then the oldest). */
static void stash_reply(TBoxTdClient *c, const char *raw) {
    lock_client(c);
    if (c->rcount == (int)(sizeof c->replies / sizeof c->replies[0])) {
        free(c->replies[c->rhead]);
        c->rhead = (c->rhead + 1) % (int)(sizeof c->replies / sizeof c->replies[0]);
        c->rcount--;
    }
    c->replies[c->rtail] = TBOX_STRDUP(raw);
    c->rtail = (c->rtail + 1) % (int)(sizeof c->replies / sizeof c->replies[0]);
    c->rcount++;
    unlock_client(c);
}

static void dispatch(TBoxTdClient *c, const char *raw) {
    cJSON *root = cJSON_Parse(raw);
    if (root == NULL) {
        return; /* malformed event: skip, never crash the loop */
    }
    if (cJSON_GetObjectItemCaseSensitive(root, "@extra") == NULL) {
        if (c->handler != NULL) {
            c->handler(c->user_data, root);
        }
    } else {
        /* a reply: stash a copy for take_reply (raw dies on next receive) */
        stash_reply(c, raw);
    }
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
#ifdef _WIN32
    InitializeCriticalSection(&c->lock);
#else
    pthread_mutex_init(&c->lock, NULL);
#endif
    api->set_verbosity(verbosity);
    c->client_id = api->create_client_id();
    return c; /* no thread yet: register a handler, then start() */
}

int tbox_td_client_start(TBoxTdClient *c) {
    if (c == NULL) {
        return -1;
    }
    if (c->started) {
        return 0;
    }
#ifdef _WIN32
    c->thread = (HANDLE)_beginthreadex(NULL, 0, recv_loop, c, 0, NULL);
    if (c->thread == NULL) {
        return -1;
    }
#else
    atomic_init(&c->closed, false);
    if (pthread_create(&c->thread, NULL, recv_loop, c) != 0) {
        return -1;
    }
#endif
    c->started = 1;
    return 0;
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

char *tbox_td_client_take_reply(TBoxTdClient *c) {
    if (c == NULL) {
        return NULL;
    }
    lock_client(c);
    char *reply = NULL;
    if (c->rcount > 0) {
        reply = c->replies[c->rhead];
        c->replies[c->rhead] = NULL; /* ownership moves to the caller;
                                        clears the slot so close() cannot
                                        free it a second time */
        c->rhead = (c->rhead + 1) % (int)(sizeof c->replies / sizeof c->replies[0]);
        c->rcount--;
    }
    unlock_client(c);
    return reply;
}

void tbox_td_client_close(TBoxTdClient *c) {
    if (c == NULL) {
        return;
    }
    if (c->started) {
#ifdef _WIN32
        InterlockedExchange(&c->closed, 1);
        /* the poll returns within a second, so this is bounded; waiting
         * forever (like the pthread_join branch) removes a use-after-free
         * window if it ever took longer than a fixed timeout */
        WaitForSingleObject(c->thread, INFINITE);
        CloseHandle(c->thread);
#else
        atomic_store(&c->closed, true);
        pthread_join(c->thread, NULL);
#endif
    }
#ifdef _WIN32
    DeleteCriticalSection(&c->lock);
#else
    pthread_mutex_destroy(&c->lock);
#endif
    for (int i = 0; i < (int)(sizeof c->replies / sizeof c->replies[0]); i++) {
        free(c->replies[i]);
    }
    free(c);
}
