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

/* Event queue limits (spec 6.6). TBOX_SESSION_EVENTS_MAX (10000) is the
 * high-water mark declared in the header; progress events are advisory and
 * coalesce instead of ever hitting it. */
#define TBOX_POLL_MS 5 /* take_event polling granularity */

struct TBoxSession {
    TBoxTdClient *client;
    long api_id;           /* session config: passed IN, never read from disk */
    char api_hash[128];
    char db_key[128];      /* base64 TDLib DB key (S5); empty = unencrypted */
    char dbdir[600];
    char filesdir[600];

    int state;             /* last TBoxAuthState reached */
    int params_sent;       /* setTdlibParameters sent (eager, once) */
    int params_asks;       /* times tdlib asked for parameters */
    int qr_sent;
    int me_sent;
    int close_sent;
    int pre_existing_db;   /* tdlib/ already existed (S5 migration branch) */
    int db_key_migrating;  /* setDatabaseEncryptionKey sent this run */

    char link[1024];       /* last QR link shown, to detect rotation */
    char conn[64];         /* last updateConnectionState seen */

    long long my_id;       /* account user id from the getMe reply */
    int is_premium;        /* account is premium (auth.ready, spec 6.5) */
    long long saved_chat;  /* Saved Messages chat id; 0 until createPrivateChat */

    /* One in-flight synchronous call (tbox_session_call, caller thread only).
     * Replies are drained by tbox_session_take_event as well, so the capture
     * happens there, keyed by the private @extra. */
    int call_seq;
    char call_extra[32];
    char *call_reply;

    /* Event queue: receive thread writes, caller thread reads. Payloads
     * are owned heap strings (P5: no fixed buffer); the queue starts
     * small and grows, so a steady trickle never pays for the cap. */
    TBoxSessionEventOut *ev;
    size_t ev_cap;
    size_t ehead;          /* oldest */
    size_t etail;          /* next write */
    size_t ecount;
    int resync_needed;     /* queue overflowed: reconcile required (6.6) */
    int progress_dropped;  /* a progress event was coalesced (6.6) */
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

/* Progress is the one kind of event spec 6.6 lets a newer copy replace. */
static int is_progress(TBoxSessionEvent type) {
    return type == TBOX_SESSION_EV_TRANSFER_PROGRESS ||
           type == TBOX_SESSION_EV_SYNC_PROGRESS;
}

/* One queue slot, owning its payload. */
typedef struct SessionSlot {
    TBoxSessionEvent type;
    int code;
    long long total;
    char *data; /* heap or NULL */
} SessionSlot;

static char *dup_text(const char *data) {
    if (data == NULL) {
        return NULL;
    }
    size_t n = strlen(data) + 1;
    char *copy = malloc(n);
    if (copy != NULL) {
        memcpy(copy, data, n);
    }
    return copy;
}

/* Grows the array before it is needed; never drops what is already there.
 * The ring is stored modulo ev_cap, so doubling the capacity reorders
 * logical positions: copy the live range out, rebase etail, and move it
 * back. Getting this wrong silently scrambles every queued event. The
 * doubling is clamped to the spec 6.6 cap, so the last growth cannot
 * overshoot it into an oversized queue. */
static int ev_grow(TBoxSession *s) {
    size_t ncap = s->ev_cap * 2;
    if (ncap > TBOX_SESSION_EVENTS_MAX) {
        ncap = TBOX_SESSION_EVENTS_MAX;
    }
    TBoxSessionEventOut *n = malloc(ncap * sizeof *n);
    if (n == NULL) {
        return -1;
    }
    for (size_t i = 0; i < s->ecount; i++) {
        n[i] = s->ev[(s->ehead + i) % s->ev_cap];
    }
    free(s->ev);
    s->ev = n;
    s->ev_cap = ncap;
    s->ehead = 0;
    s->etail = s->ecount;
    return 0;
}

/* Where a new event's storage lives, lock held. The queue grows by
 * doubling until it would pass the spec 6.6 high-water mark, and only
 * then reports that it cannot take more: an event that cannot be queued
 * does not vanish quietly - it sets resync_needed and returns NULL so the
 * caller can fall back to the reconcile hint (spec 6.6). */
static TBoxSessionEventOut *enqueue_locked(TBoxSession *s, TBoxSessionEvent type) {
    if (s->ecount == s->ev_cap) {
        if (s->ev_cap >= TBOX_SESSION_EVENTS_MAX || ev_grow(s) != 0) {
            s->resync_needed = 1;
            return NULL;
        }
    }
    TBoxSessionEventOut *out = &s->ev[s->etail];
    out->type = type;
    out->code = 0;
    out->total = 0;
    out->data = NULL;
    s->etail = (s->etail + 1) % s->ev_cap;
    s->ecount++;
    return out;
}

/* Queue one event, heap payload frees its predecessor when coalescing.
 * When the queue refuses the event (memory), everything worth keeping is
 * still delivered: the coalesced progress is already gone, and a resync
 * hint goes in its place. An event pushed INSIDE this failure path is
 * never itself refused - the queue is not fuller than before. */
static void push_event_full(TBoxSession *s, TBoxSessionEvent type, int code,
                            long long total, const char *data) {
    lock_session(s);
    if (is_progress(type) && data != NULL) {
        /* coalescing: keep only the latest progress per op_id */
        if (s->ecount >= 2) {
            TBoxSessionEventOut *newest =
                &s->ev[(s->etail + s->ev_cap - 1) % s->ev_cap];
            if (newest->type == type && newest->data != NULL &&
                strcmp(newest->data, data) == 0) {
                newest->code = code;
                newest->total = total;
                s->progress_dropped = 1;
                unlock_session(s);
                return;
            }
        }
        /* a burst cannot flood the queue with one op's stale progress */
        if (s->ecount >= TBOX_SESSION_EVENTS_MAX) {
            s->progress_dropped = 1;
            unlock_session(s);
            return;
        }
    }
    TBoxSessionEventOut *out = enqueue_locked(s, type);
    if (out == NULL) {
        /* Overflow: the event itself is replaced by the reconcile hint
         * (spec 6.6), which every front already knows how to handle. */
        out = enqueue_locked(s, TBOX_SESSION_EV_CATALOG_CHANGED);
        if (out != NULL) {
            out->data = dup_text("*");
        }
        unlock_session(s);
        return;
    }
    out->code = code;
    out->total = total;
    out->data = dup_text(data);
    unlock_session(s);
}

/* The common text event. */
static void push_event(TBoxSession *s, TBoxSessionEvent type, int code,
                       const char *data) {
    push_event_full(s, type, code, 0, data);
}

static int pop_event(TBoxSession *s, TBoxSessionEventOut *out) {
    lock_session(s);
    int have = s->ecount > 0;
    if (have) {
        *out = s->ev[s->ehead];         /* the payload moves to the caller */
        s->ev[s->ehead].data = NULL;    /* so close() cannot free it again */
        s->ehead = (s->ehead + 1) % s->ev_cap;
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
    if (s->db_key[0] != '\0') {
        /* S5: the TDLib DB is encrypted at rest; the key is a secret and
         * never appears anywhere but this request. */
        cJSON_AddStringToObject(req, "database_encryption_key", s->db_key);
    }
    cJSON_AddStringToObject(req, "device_model", "tbox");
    cJSON_AddStringToObject(req, "system_language_code", "en");
    cJSON_AddStringToObject(req, "application_version", "0.1.0-dev");
    cJSON_AddStringToObject(req, "@extra", "init");
    session_send(s, req);
}

/* ---- tdlib callbacks ------------------------------------------------ */

/* The chat a catalog update belongs to: the top-level chat_id, or the
 * message's chat_id for updateNewMessage / updateMessageSendSucceeded. 0
 * when the update names no chat. */
static long long update_chat_id(const cJSON *update) {
    const cJSON *chat = cJSON_GetObjectItemCaseSensitive(update, "chat_id");
    if (cJSON_IsNumber(chat)) {
        return (long long)chat->valuedouble;
    }
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(update, "message");
    const cJSON *mc = cJSON_GetObjectItemCaseSensitive(msg, "chat_id");
    if (cJSON_IsNumber(mc)) {
        return (long long)mc->valuedouble;
    }
    return 0;
}

/* The updates src/sync/updates.c acts on (spec 8.3). Everything else stays
 * out of the session queue, so a busy account cannot crowd it. */
static int is_catalog_update(const char *t) {
    return strcmp(t, "updateNewMessage") == 0 ||
           strcmp(t, "updateMessageContent") == 0 ||
           strcmp(t, "updateMessageEdited") == 0 ||
           strcmp(t, "updateDeleteMessages") == 0 ||
           strcmp(t, "updateMessageSendSucceeded") == 0;
}

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

    if (is_catalog_update(t)) {
        /* spec 8.3: the Saved Messages chat only. Before createPrivateChat
         * answers there is no id to compare against; the startup scan fills
         * that gap. */
        if (s->saved_chat != 0 && update_chat_id(update) == s->saved_chat) {
            char *raw = cJSON_PrintUnformatted(update);
            if (raw != NULL) {
                push_event(s, TBOX_SESSION_EV_UPDATE, 0, raw);
                cJSON_free(raw);
            }
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
 * the getMe answer as the account name and id. A reply to a synchronous
 * tbox_session_call is captured for that caller instead. */
static void session_drain_replies(TBoxSession *s) {
    char *raw = NULL;
    while ((raw = tbox_td_client_take_reply(s->client)) != NULL) {
        cJSON *root = cJSON_Parse(raw);
        if (root != NULL) {
            const cJSON *extra = cJSON_GetObjectItemCaseSensitive(root, "@extra");
            const char *ex = cJSON_IsString(extra) ? cJSON_GetStringValue(extra) : "";
            if (s->call_extra[0] != '\0' && strcmp(ex, s->call_extra) == 0) {
                /* a synchronous call owns this reply; hand it over intact */
                if (s->call_reply == NULL) {
                    s->call_reply = raw;
                    raw = NULL;
                }
            } else {
                const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "@type");
                const char *t = cJSON_IsString(type) ? cJSON_GetStringValue(type) : "";
                if (strcmp(t, "error") == 0) {
                    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
                    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
                    push_event(s, TBOX_SESSION_EV_ERROR,
                               cJSON_IsNumber(code) ? (int)code->valuedouble : 0,
                               cJSON_IsString(msg) ? cJSON_GetStringValue(msg) : "");
                } else if (strcmp(t, "user") == 0) {
                    if (strcmp(ex, "me") == 0) {
                        const char *first = cJSON_GetStringValue(
                            cJSON_GetObjectItemCaseSensitive(root, "first_name"));
                        const char *last = cJSON_GetStringValue(
                            cJSON_GetObjectItemCaseSensitive(root, "last_name"));
                        const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
                        const cJSON *premium =
                            cJSON_GetObjectItemCaseSensitive(root, "is_premium");
                        if (cJSON_IsNumber(id)) {
                            s->my_id = (long long)id->valuedouble;
                        }
                        if (cJSON_IsTrue(premium)) {
                            s->is_premium = 1;
                        }
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
                                long api_id, const char *api_hash,
                                const char *db_key) {
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
    if (db_key != NULL) {
        snprintf(s->db_key, sizeof s->db_key, "%s", db_key);
    }
    join_dir(s->dbdir, sizeof s->dbdir, session_dir, "tdlib");
    join_dir(s->filesdir, sizeof s->filesdir, session_dir, "files");
    /* S5: if tdlib/ already exists this is an upgraded profile whose DB may
     * predate encryption; the engine migrates it after auth.ready instead
     * of assuming setTdlibParameters already covered it. Detect BEFORE
     * mkdirs creates it. */
    s->pre_existing_db = tbox_fsx_is_dir(s->dbdir);
    /* tdlib creates its database directory but NOT its parent, so a
     * fresh "session/tdlib" failed with "Can't create directory" until
     * the parent existed. Existence is fine; a failure surfaces through
     * tdlib's own error, so the status is not checked here. */
    (void)tbox_fsx_mkdirs(s->dbdir);
    (void)tbox_fsx_mkdirs(s->filesdir);
    s->state = (int)TBOX_AUTH_START;

    /* start small; the queue grows on demand (spec 6.6) */
    s->ev_cap = 8;
    s->ev = calloc(s->ev_cap, sizeof *s->ev);
    if (s->ev == NULL) {
        goto fail;
    }

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
    free(s->ev);
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

int tbox_session_resync_needed(TBoxSession *s) {
    if (s == NULL) {
        return 0;
    }
    lock_session(s);
    int v = s->resync_needed;
    unlock_session(s);
    return v;
}

int tbox_session_progress_dropped(TBoxSession *s) {
    if (s == NULL) {
        return 0;
    }
    lock_session(s);
    int v = s->progress_dropped;
    unlock_session(s);
    return v;
}

long long tbox_session_my_id(const TBoxSession *s) {
    return (s != NULL) ? s->my_id : 0;
}

int tbox_session_is_premium(const TBoxSession *s) {
    return (s != NULL) ? s->is_premium : 0;
}

void tbox_session_set_saved_chat(TBoxSession *s, long long chat_id) {
    if (s != NULL) {
        s->saved_chat = chat_id;
    }
}

char *tbox_session_call(TBoxSession *s, const char *request_json,
                        unsigned timeout_ms) {
    if (s == NULL || request_json == NULL) {
        return NULL;
    }
    cJSON *req = cJSON_Parse(request_json);
    if (req == NULL || !cJSON_IsObject(req)) {
        cJSON_Delete(req);
        return NULL;
    }
    /* A private @extra so this reply cannot be confused with the auth flow's
     * ("init", "me", "qr", ...). */
    s->call_seq++;
    snprintf(s->call_extra, sizeof s->call_extra, "call:%d", s->call_seq);
    cJSON_DeleteItemFromObjectCaseSensitive(req, "@extra");
    cJSON_AddStringToObject(req, "@extra", s->call_extra);
    free(s->call_reply);
    s->call_reply = NULL;
    session_send(s, req); /* takes ownership */

    unsigned waited = 0;
    for (;;) {
        session_drain_replies(s);
        if (s->call_reply != NULL) {
            char *reply = s->call_reply;
            s->call_reply = NULL;
            s->call_extra[0] = '\0';
            return reply;
        }
        if (waited >= timeout_ms) {
            s->call_extra[0] = '\0';
            return NULL;
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

int tbox_session_had_existing_db(const TBoxSession *s) {
    return (s != NULL) ? s->pre_existing_db : 0;
}

void tbox_session_encrypt_db(TBoxSession *s, const char *db_key) {
    if (s == NULL || db_key == NULL || db_key[0] == '\0' || s->db_key_migrating) {
        return;
    }
    s->db_key_migrating = 1;
    /* S5: encrypt an existing, previously-unencrypted database. The key is
     * a secret and appears nowhere but this request. */
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "@type", "setDatabaseEncryptionKey");
    cJSON_AddStringToObject(req, "new_encryption_key", db_key);
    cJSON_AddStringToObject(req, "@extra", "dbkey");
    session_send(s, req);
}

void tbox_session_logout(TBoxSession *s) {
    if (s == NULL) {
        return;
    }
    /* S10: TDLib logOut. It removes the session on Telegram's side and
     * leads to authorizationStateClosed; it deletes nothing the user
     * stored. */
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "@type", "logOut");
    cJSON_AddStringToObject(req, "@extra", "logout");
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
    /* payloads still queued belong to the session until taken; drop them */
    for (size_t i = 0; i < s->ecount; i++) {
        free(s->ev[(s->ehead + i) % s->ev_cap].data);
    }
    free(s->ev);
    free(s->call_reply);
#ifdef _WIN32
    DeleteCriticalSection(&s->lock);
#else
    pthread_mutex_destroy(&s->lock);
#endif
    free(s);
}