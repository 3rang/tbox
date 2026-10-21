/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * tbox_session.c — drives one tdlib session without any user interface.
 *
 * Everything the old login command did for the user (printing, prompting,
 * painting the QR, heartbeats, watchdog timeouts) is gone; what remains is
 * the part only the engine can do: make the session directory, talk the
 * authorizationState protocol, translate replies into events. Timeouts and
 * wording belong to the front, so no clock lives here either.
 */
#include "service/tbox_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define TBOX_SLEEP_MS(ms) Sleep(ms)
#else
#include <pthread.h>
#include <unistd.h>
#define TBOX_SLEEP_MS(ms) usleep((ms) * 1000)
#endif

#include "cJSON.h"
#include "td/tbox_td_auth.h"
#include "td/tbox_td_client.h"
#include "util/fsx.h"

#define TBOX_EVQ 32     /* pending events before the oldest is dropped */
#define TBOX_POLL_MS 5  /* take_event polling granularity */

struct TBoxSession {
    TBoxTdClient *client;
    long api_id;           /* session config: passed IN, never read from disk */
    char api_hash[128];
    char dbdir[600];
    char filesdir[600];

    int state;             /* last TBoxAuthState reached */
    int params_sent;       /* setTdlibParameters sent (eager, once) */
    int params_asks;       /* times tdlib asked for parameters */
    int qr_sent;
    int me_sent;
    int close_sent;

    char link[1024];       /* last QR link shown, to detect rotation */
    char conn[64];         /* last updateConnectionState seen */

    /* Event ring: receive thread writes, caller thread reads. */
    TBoxSessionEventOut ev[TBOX_EVQ];
    int ehead;             /* oldest */
    int etail;             /* next write */
    int ecount;
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
};

/* ---- small plumbing ------------------------------------------------- */

static void lock_session(TBoxSession *s) {
#ifdef _WIN32
    EnterCriticalSection(&s->lock);
#else
    pthread_mutex_lock(&s->lock);
#endif
}

static void unlock_session(TBoxSession *s) {
#ifdef _WIN32
    LeaveCriticalSection(&s->lock);
#else
    pthread_mutex_unlock(&s->lock);
#endif
}

/* Queue one event; when the ring is full the OLDEST is dropped, so a front
 * that stopped reading still sees where the session ended up. */
static void push_event(TBoxSession *s, TBoxSessionEvent type, int code,
                       const char *data) {
    lock_session(s);
    if (s->ecount == TBOX_EVQ) {
        s->ehead = (s->ehead + 1) % TBOX_EVQ;
        s->ecount--;
    }
    TBoxSessionEventOut *out = &s->ev[s->etail];
    out->type = type;
    out->code = code;
    snprintf(out->data, sizeof out->data, "%s", data != NULL ? data : "");
    s->etail = (s->etail + 1) % TBOX_EVQ;
    s->ecount++;
    unlock_session(s);
}

static int pop_event(TBoxSession *s, TBoxSessionEventOut *out) {
    lock_session(s);
    int have = s->ecount > 0;
    if (have) {
        *out = s->ev[s->ehead];
        s->ehead = (s->ehead + 1) % TBOX_EVQ;
        s->ecount--;
    }
    unlock_session(s);
    return have;
}

static void session_send(TBoxSession *s, cJSON *req) {
    char *json = cJSON_PrintUnformatted(req);
    if (json != NULL) {
        tbox_td_client_send(s->client, json);
        cJSON_free(json);
    }
    cJSON_Delete(req);
}

/* Sends setTdlibParameters (proven shape; db + files persist). Called
 * eagerly at startup - this tdlib build does not always emit the opening
 * WaitTdlibParameters cue - and again if that state shows up unbidden. */
static void session_send_params(TBoxSession *s) {
    s->params_sent = 1;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "@type", "setTdlibParameters");
    cJSON_AddNumberToObject(req, "api_id", (double)s->api_id);
    cJSON_AddStringToObject(req, "api_hash", s->api_hash);
    cJSON_AddBoolToObject(req, "use_test_dc", 0);
    cJSON_AddStringToObject(req, "database_directory", s->dbdir);
    cJSON_AddStringToObject(req, "files_directory", s->filesdir);
    cJSON_AddBoolToObject(req, "use_file_database", 1);
    cJSON_AddBoolToObject(req, "use_message_database", 1);
    cJSON_AddBoolToObject(req, "use_secret_chats", 0);
    cJSON_AddStringToObject(req, "device_model", "tbox");
    cJSON_AddStringToObject(req, "system_language_code", "en");
    cJSON_AddStringToObject(req, "application_version", "0.1.0-dev");
    cJSON_AddStringToObject(req, "@extra", "init");
    session_send(s, req);
}

/* ---- tdlib callbacks ------------------------------------------------ */

static void session_update(void *user_data, const cJSON *update) {
    TBoxSession *s = user_data;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(update, "@type");
    const char *t = cJSON_IsString(type) ? cJSON_GetStringValue(type) : "";

    /* Network visibility is state, not decoration: the front decides how
     * (or whether) to show it. */
    if (strcmp(t, "updateConnectionState") == 0) {
        const cJSON *cs = cJSON_GetObjectItemCaseSensitive(update, "state");
        const cJSON *cs_type = cJSON_GetObjectItemCaseSensitive(cs, "@type");
        const char *name = cJSON_IsString(cs_type) ? cJSON_GetStringValue(cs_type) : "";
        if (strcmp(name, s->conn) != 0) {
            snprintf(s->conn, sizeof s->conn, "%s", name);
            push_event(s, TBOX_SESSION_EV_CONNECTION, 0, name);
        }
        return;
    }

    if (strcmp(t, "updateAuthorizationState") != 0) {
        return; /* only auth updates drive the session */
    }

    const cJSON *as = cJSON_GetObjectItemCaseSensitive(update, "authorization_state");
    const cJSON *as_type = cJSON_GetObjectItemCaseSensitive(as, "@type");
    const char *st = cJSON_IsString(as_type) ? cJSON_GetStringValue(as_type) : NULL;

    TBoxAuthTransition tr = tbox_auth_next((TBoxAuthState)s->state, st);
    if (tr.next == TBOX_AUTH_FAILED) {
        push_event(s, TBOX_SESSION_EV_ERROR, 0,
                   st != NULL ? st : "unknown authorization state");
        return;
    }
    s->state = (int)tr.next;
    push_event(s, TBOX_SESSION_EV_AUTH_STATE, 0, tbox_auth_state_name(tr.next));

    switch (tr.action) {
    case TBOX_AUTH_ACT_REQUEST_PARAMS:
        s->params_asks++;
        if (!s->params_sent) {
            session_send_params(s);
        } else if (s->params_asks > 1) {
            /* Asking twice after our eager send means tdlib rejected the
             * parameters. Re-sending earns "Unexpected setTdlibParameters",
             * so report it and let the front surface it. */
            push_event(s, TBOX_SESSION_EV_ERROR, 0,
                       "tdlib keeps asking for parameters; check the api-id/api-hash pair");
        }
        break;
    case TBOX_AUTH_ACT_REQUEST_QR:
        if (!s->qr_sent) {
            s->qr_sent = 1;
            cJSON *req = cJSON_CreateObject();
            cJSON_AddStringToObject(req, "@type", "requestQrCodeAuthentication");
            cJSON_AddItemToObject(req, "other_user_ids", cJSON_CreateArray());
            cJSON_AddStringToObject(req, "@extra", "qr");
            session_send(s, req);
        }
        break;
    case TBOX_AUTH_ACT_SHOW_QR: {
        const char *link = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(as, "link"));
        if (link != NULL && strlen(link) < sizeof s->link &&
            strcmp(link, s->link) != 0) {
            snprintf(s->link, sizeof s->link, "%s", link);
            push_event(s, TBOX_SESSION_EV_QR_LINK, 0, link);
        }
        break;
    }
    case TBOX_AUTH_ACT_ASK_PASSWORD: {
        const char *hint = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(as, "password_hint"));
        push_event(s, TBOX_SESSION_EV_PASSWORD_NEEDED, 0, hint);
        break;
    }
    case TBOX_AUTH_ACT_AUTHORIZED:
        if (!s->me_sent) {
            s->me_sent = 1;
            cJSON *req = cJSON_CreateObject();
            cJSON_AddStringToObject(req, "@type", "getMe");
            cJSON_AddStringToObject(req, "@extra", "me");
            session_send(s, req);
        }
        break;
    case TBOX_AUTH_ACT_CLOSED:
        push_event(s, TBOX_SESSION_EV_CLOSED, 0, "");
        break;
    case TBOX_AUTH_ACT_FATAL:
        push_event(s, TBOX_SESSION_EV_ERROR, 0, "cannot continue login");
        break;
    case TBOX_AUTH_ACT_NONE:
    default:
        break;
    }
}

/* Replies (events carrying @extra) become events too: errors verbatim, and
 * the getMe answer as the account name. "ok" needs no translation. */
static void session_drain_replies(TBoxSession *s) {
    char *raw = NULL;
    while ((raw = tbox_td_client_take_reply(s->client)) != NULL) {
        cJSON *root = cJSON_Parse(raw);
        if (root != NULL) {
            const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "@type");
            const char *t = cJSON_IsString(type) ? cJSON_GetStringValue(type) : "";
            if (strcmp(t, "error") == 0) {
                const cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
                const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
                push_event(s, TBOX_SESSION_EV_ERROR,
                           cJSON_IsNumber(code) ? (int)code->valuedouble : 0,
                           cJSON_IsString(msg) ? cJSON_GetStringValue(msg) : "");
            } else if (strcmp(t, "user") == 0) {
                const cJSON *extra = cJSON_GetObjectItemCaseSensitive(root, "@extra");
                if (cJSON_IsString(extra) &&
                    strcmp(cJSON_GetStringValue(extra), "me") == 0) {
                    const char *first = cJSON_GetStringValue(
                        cJSON_GetObjectItemCaseSensitive(root, "first_name"));
                    const char *last = cJSON_GetStringValue(
                        cJSON_GetObjectItemCaseSensitive(root, "last_name"));
                    char name[256];
                    snprintf(name, sizeof name, "%s %s", first ? first : "",
                             last ? last : "");
                    size_t n = strlen(name);
                    while (n > 0 && name[n - 1] == ' ') {
                        name[--n] = '\0';
                    }
                    push_event(s, TBOX_SESSION_EV_READY, 0, name);
                }
            }
            cJSON_Delete(root);
        }
        free(raw);
    }
}

/* ---- paths ---------------------------------------------------------- */

/* Strips trailing '/\' so "session\" and "session" give the same paths. */
static void join_dir(char *dst, size_t cap, const char *base, const char *leaf) {
    char clean[512];
    snprintf(clean, sizeof clean, "%s", base);
    size_t n = strlen(clean);
    while (n > 0 && (clean[n - 1] == '/' || clean[n - 1] == '\\')) {
        clean[--n] = '\0';
    }
    snprintf(dst, cap, "%s/%s", clean, leaf);
}

/* ---- public API ----------------------------------------------------- */

TBoxSession *tbox_session_start(const TBoxTdApi *api, const char *session_dir,
                                long api_id, const char *api_hash) {
    if (api == NULL || session_dir == NULL || session_dir[0] == '\0' ||
        api_id <= 0 || api_hash == NULL || api_hash[0] == '\0') {
        return NULL;
    }
    TBoxSession *s = calloc(1, sizeof(TBoxSession));
    if (s == NULL) {
        return NULL;
    }
#ifdef _WIN32
    InitializeCriticalSection(&s->lock);
#else
    pthread_mutex_init(&s->lock, NULL);
#endif
    s->api_id = api_id;
    snprintf(s->api_hash, sizeof s->api_hash, "%s", api_hash);
    join_dir(s->dbdir, sizeof s->dbdir, session_dir, "tdlib");
    join_dir(s->filesdir, sizeof s->filesdir, session_dir, "files");
    /* tdlib creates its database directory but NOT its parent, so a
     * fresh "session/tdlib" failed with "Can't create directory" until
     * the parent existed. Existence is fine; a failure surfaces through
     * tdlib's own error, so the status is not checked here. */
    (void)tbox_fsx_mkdirs(s->dbdir);
    (void)tbox_fsx_mkdirs(s->filesdir);
    s->state = (int)TBOX_AUTH_START;

    s->client = tbox_td_client_open(api, 0);
    if (s->client == NULL) {
        goto fail;
    }
    tbox_td_client_on_update(s->client, session_update, s);
    if (tbox_td_client_start(s->client) != 0) {
        tbox_td_client_close(s->client);
        s->client = NULL;
        goto fail;
    }
    session_send_params(s); /* eager, before any cue can be missed */
    return s;

fail:
#ifdef _WIN32
    DeleteCriticalSection(&s->lock);
#else
    pthread_mutex_destroy(&s->lock);
#endif
    free(s);
    return NULL;
}

int tbox_session_take_event(TBoxSession *s, TBoxSessionEventOut *out,
                            unsigned timeout_ms) {
    if (s == NULL || out == NULL) {
        return 0;
    }
    unsigned waited = 0;
    for (;;) {
        session_drain_replies(s);
        if (pop_event(s, out)) {
            return 1;
        }
        if (waited >= timeout_ms) {
            return 0;
        }
        TBOX_SLEEP_MS(TBOX_POLL_MS);
        waited += TBOX_POLL_MS;
    }
}

void tbox_session_submit_password(TBoxSession *s, const char *password) {
    if (s == NULL || password == NULL) {
        return;
    }
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "@type", "checkAuthenticationPassword");
    cJSON_AddStringToObject(req, "password", password);
    cJSON_AddStringToObject(req, "@extra", "pwd");
    session_send(s, req);
}

void tbox_session_request_close(TBoxSession *s) {
    if (s == NULL || s->close_sent) {
        return;
    }
    s->close_sent = 1;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "@type", "close");
    session_send(s, req);
}

void tbox_session_close(TBoxSession *s) {
    if (s == NULL) {
        return;
    }
    tbox_td_client_close(s->client); /* stops the thread: no more pushes */
#ifdef _WIN32
    DeleteCriticalSection(&s->lock);
#else
    pthread_mutex_destroy(&s->lock);
#endif
    free(s);
}