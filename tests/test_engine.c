/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * test_engine.c — the engine loop over the fake TDLib (spec 0.6).
 *
 * Drives the real loop in a worker thread, feeds protocol lines through
 * tbox_engine_push_command() and captures every emitted line, exactly as
 * the stdio transport would. No dll, no network.
 */
#undef NDEBUG /* keep assert() alive in Release (/DNDEBUG) builds */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/engine.h"
#include "engine/protocol.h"
#include "util/fsx.h"

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define T_SLEEP(ms) Sleep(ms)
#else
#include <pthread.h>
#include <unistd.h>
#define T_SLEEP(ms) usleep((ms) * 1000)
#endif

#include "fake_td.h"

static const char *DIR = "tbox_engine_test";

/* ---- captured output ------------------------------------------------- */

#define OUT_MAX 512
typedef struct Out {
    char *line[OUT_MAX];
    int n;
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
} Out;

static void out_lock(Out *o) {
#ifdef _WIN32
    EnterCriticalSection(&o->lock);
#else
    pthread_mutex_lock(&o->lock);
#endif
}
static void out_unlock(Out *o) {
#ifdef _WIN32
    LeaveCriticalSection(&o->lock);
#else
    pthread_mutex_unlock(&o->lock);
#endif
}

static void capture(const char *json_line, void *ctx) {
    Out *o = ctx;
    out_lock(o);
    if (o->n < OUT_MAX) {
        o->line[o->n++] = _strdup(json_line);
    }
    out_unlock(o);
}

/* Returns the first captured line containing `needle`, or NULL. */
static const char *find_line(Out *o, const char *needle) {
    out_lock(o);
    for (int i = 0; i < o->n; i++) {
        if (strstr(o->line[i], needle) != NULL) {
            out_unlock(o);
            return o->line[i]; /* stable while the test holds no lock */
        }
    }
    out_unlock(o);
    return NULL;
}

static void out_free(Out *o) {
    for (int i = 0; i < o->n; i++) {
        free(o->line[i]);
    }
#ifdef _WIN32
    DeleteCriticalSection(&o->lock);
#else
    pthread_mutex_destroy(&o->lock);
#endif
}

/* Waits until a captured line contains `needle`; returns it or NULL. */
static const char *wait_for(Out *o, const char *needle, unsigned ms) {
    unsigned waited = 0;
    for (;;) {
        const char *l = find_line(o, needle);
        if (l != NULL) {
            return l;
        }
        if (waited >= ms) {
            return NULL;
        }
        T_SLEEP(5);
        waited += 5;
    }
}

/* True if the fake recorded a sent request containing `needle`. */
static int sent_has(const FakeTd *f, const char *needle) {
    for (int i = 0; i < f->sent_n; i++) {
        if (strstr(f->sent[i], needle) != NULL) {
            return 1;
        }
    }
    return 0;
}

/* ---- engine thread --------------------------------------------------- */

typedef struct Run {
    TBoxEngine *e;
    int rc;
#ifdef _WIN32
    HANDLE thread;
#else
    pthread_t thread;
#endif
} Run;

#ifdef _WIN32
static unsigned __stdcall run_main(void *arg) {
#else
static void *run_main(void *arg) {
#endif
    Run *r = arg;
    r->rc = tbox_engine_run(r->e);
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

static void run_in_thread(Run *r) {
#ifdef _WIN32
    uintptr_t h = _beginthreadex(NULL, 0, run_main, r, 0, NULL);
    assert(h != 0);
    r->thread = (HANDLE)h;
#else
    pthread_create(&r->thread, NULL, run_main, r);
#endif
}

static void join_thread(Run *r) {
#ifdef _WIN32
    WaitForSingleObject(r->thread, 15000);
    CloseHandle(r->thread);
#else
    pthread_join(r->thread, NULL);
#endif
}

static const TBoxTdApi *API(void) {
    static FakeTd *f = NULL;
    if (f == NULL) {
        f = fake_td_start(42);
    }
    return &f->api;
}

int main(void) {
    assert(tbox_fsx_mkdirs(DIR) == TBOX_FSX_OK);

    Out out;
    memset(&out, 0, sizeof out);
#ifdef _WIN32
    InitializeCriticalSection(&out.lock);
#else
    pthread_mutex_init(&out.lock, NULL);
#endif

    TBoxEngineConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.profile_dir = DIR;

    int err = -1;
    TBoxEngine *e = tbox_engine_new(API(), &cfg, capture, &out, &err);
    assert(e != NULL);
    assert(err == 0);

    Run r;
    memset(&r, 0, sizeof r);
    r.e = e;
    run_in_thread(&r);

    /* 1. hello: version + protocol, id echoed */
    tbox_engine_push_command(e, "{\"id\":1,\"cmd\":\"hello\",\"args\":{\"protocol\":1}}");
    const char *l = wait_for(&out, "\"engine_version\"", 3000);
    assert(l != NULL);
    assert(strstr(l, "\"id\":1") != NULL);
    assert(strstr(l, "\"protocol\":1") != NULL);

    /* 2. wrong protocol version -> PROTOCOL_ERROR */
    tbox_engine_push_command(e, "{\"id\":2,\"cmd\":\"hello\",\"args\":{\"protocol\":2}}");
    l = wait_for(&out, "\"id\":2", 3000);
    assert(l != NULL && strstr(l, "PROTOCOL_ERROR") != NULL);

    /* 3. unknown command */
    tbox_engine_push_command(e, "{\"id\":3,\"cmd\":\"bogus\"}");
    l = wait_for(&out, "\"id\":3", 3000);
    assert(l != NULL && strstr(l, "UNKNOWN_COMMAND") != NULL);

    /* 4. malformed line with an id -> BAD_ARGS; without -> dropped */
    tbox_engine_push_command(e, "{\"id\":4}");
    l = wait_for(&out, "\"id\":4", 3000);
    assert(l != NULL && strstr(l, "BAD_ARGS") != NULL);
    int before = out.n;
    tbox_engine_push_command(e, "this is not json");
    T_SLEEP(50);
    assert(out.n == before); /* unanswerable lines produce nothing */

    /* 5. auth.password before auth.start -> NOT_AUTHORIZED */
    tbox_engine_push_command(e, "{\"id\":5,\"cmd\":\"auth.password\",\"args\":{\"password\":\"x\"}}");
    l = wait_for(&out, "\"id\":5", 3000);
    assert(l != NULL && strstr(l, "NOT_AUTHORIZED") != NULL);

    /* 6. auth.start without args -> BAD_ARGS */
    tbox_engine_push_command(e, "{\"id\":6,\"cmd\":\"auth.start\",\"args\":{}}");
    l = wait_for(&out, "\"id\":6", 3000);
    assert(l != NULL && strstr(l, "BAD_ARGS") != NULL);

    /* 7. auth.start -> ok, then the auth flow turns into events */
    tbox_engine_push_command(e,
        "{\"id\":7,\"cmd\":\"auth.start\",\"args\":{\"api_id\":\"123\","
        "\"api_hash\":\"deadbeef\",\"db_key\":\"a2V5\"}}");
    l = wait_for(&out, "\"id\":7", 3000);
    assert(l != NULL && strstr(l, "\"ok\":true") != NULL);

    FakeTd *fake = (FakeTd *)API(); /* same singleton the engine uses */

    /* setTdlibParameters went out eagerly with the injected db key (S5) */
    T_SLEEP(100);
    assert(sent_has(fake, "setTdlibParameters") == 1);
    assert(sent_has(fake, "a2V5") == 1); /* the db_key value travelled */

    /* 7b. auth updates become protocol events */
    fake_td_enqueue(fake, "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":\"authorizationStateWaitPhoneNumber\"}}");
    l = wait_for(&out, "\"event\":\"auth.state\"", 3000);
    assert(l != NULL && strstr(l, "WAIT_PHONE") != NULL);

    fake_td_enqueue(fake, "{\"@type\":\"updateConnectionState\",\"state\":{\"@type\":\"connectionStateReady\"}}");
    l = wait_for(&out, "\"event\":\"conn.state\"", 3000);
    assert(l != NULL && strstr(l, "connectionStateReady") != NULL);

    fake_td_enqueue(fake, "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":\"authorizationStateWaitOtherDeviceConfirmation\",\"link\":\"tg://login?token=ZZZ\"}}");
    l = wait_for(&out, "\"event\":\"auth.qr\"", 3000);
    assert(l != NULL && strstr(l, "tg://login?token=ZZZ") != NULL);

    /* 2FA: hint out, password in */
    fake_td_enqueue(fake, "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":\"authorizationStateWaitPassword\",\"password_hint\":\"hint\"}}");
    assert(wait_for(&out, "\"event\":\"auth.password_needed\"", 3000) != NULL);
    tbox_engine_push_command(e, "{\"id\":20,\"cmd\":\"auth.password\",\"args\":{\"password\":\"secret\"}}");
    assert(wait_for(&out, "\"id\":20", 3000) != NULL);
    T_SLEEP(100);
    assert(sent_has(fake, "checkAuthenticationPassword") == 1);

    /* ready: account + observed state */
    fake_td_enqueue(fake, "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":\"authorizationStateReady\"}}");
    assert(wait_for(&out, "\"event\":\"auth.state\"", 3000) != NULL);
    T_SLEEP(100);

    /* tdlib error reply -> engine.error with code */
    fake_td_enqueue(fake, "{\"@type\":\"error\",\"@extra\":\"qr\",\"code\":400,\"message\":\"API_ID_INVALID\"}");
    l = wait_for(&out, "\"event\":\"engine.error\"", 3000);
    assert(l != NULL && strstr(l, "API_ID_INVALID") != NULL && strstr(l, "400") != NULL);

    /* auth.start twice -> BAD_ARGS */
    tbox_engine_push_command(e, "{\"id\":8,\"cmd\":\"auth.start\",\"args\":{\"api_id\":\"1\",\"api_hash\":\"x\"}}");
    l = wait_for(&out, "\"id\":8", 3000);
    assert(l != NULL && strstr(l, "BAD_ARGS") != NULL);

    /* hello again still works on a live engine */
    tbox_engine_push_command(e, "{\"id\":9,\"cmd\":\"hello\",\"args\":{\"protocol\":1}}");
    assert(wait_for(&out, "\"id\":9", 3000) != NULL);

    /* 10. engine.status reflects observed auth state */
    tbox_engine_push_command(e, "{\"id\":10,\"cmd\":\"engine.status\"}");
    l = wait_for(&out, "\"id\":10", 3000);
    assert(l != NULL && strstr(l, "\"auth_state\"") != NULL);
    assert(strstr(l, "READY") != NULL); /* the state the events set */

    /* 11. engine.shutdown -> ok, loop exits 0.
     * First let tdlib say Closed, as it would after a real close, so the
     * bounded wait in shutdown returns at once. */
    fake_td_enqueue(fake, "{\"@type\":\"updateAuthorizationState\",\"authorization_state\":{\"@type\":\"authorizationStateClosed\"}}");
    assert(wait_for(&out, "\"event\":\"auth.closed\"", 3000) != NULL);
    tbox_engine_push_command(e, "{\"id\":11,\"cmd\":\"engine.shutdown\"}");
    l = wait_for(&out, "\"id\":11", 3000);
    assert(l != NULL && strstr(l, "\"ok\":true") != NULL);
    join_thread(&r);
    assert(r.rc == 0);

    tbox_engine_free(e);
    out_free(&out);

    /* profile busy: a second engine on a held lock gets err 2 */
    TBoxFsxLock *held = NULL;
    assert(tbox_fsx_lock(DIR, &held) == TBOX_FSX_OK);
    TBoxEngine *e2 = tbox_engine_new(API(), &cfg, NULL, NULL, &err);
    assert(e2 == NULL && err == 2);
    tbox_fsx_unlock(held);

    printf("engine OK\n");
    return 0;
}
