/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * engine.c — the engine loop (spec 3.1 rule 2: one thread owns state).
 *
 * This file is the only place that decides what a command means. It does
 * no I/O of its own: commands arrive through tbox_engine_push_command()
 * (called by the transport's stdin reader) and every line that leaves goes
 * out through the emit callback. That split is what lets a future mobile
 * or Flutter front embed the engine without the stdio transport.
 */
#include "engine/engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "catalog/resolve.h"
#include "cmd/cmd.h" /* TBOX_VERSION */
#include "engine/items.h"
#include "engine/protocol.h"
#include "service/tbox_session.h"
#include "store/db.h"
#include "store/query.h"
#include "sync/scan.h"
#include "sync/updates.h"
#include "util/fsx.h"
#include "util/log.h"

#ifdef _WIN32
#include <windows.h>
#define TBOX_SLEEP_MS(ms) Sleep(ms)
#define TBOX_STRDUP _strdup
#else
#include <pthread.h>
#include <unistd.h>
#define TBOX_SLEEP_MS(ms) usleep((ms) * 1000)
#define TBOX_STRDUP strdup
#endif

#define TBOX_ENGINE_POLL_MS 5 /* loop tick: session events + shutdown check */

/* How long one synchronous TDLib call may take. A history page answers in
 * well under a second; the margin is for a slow link. */
#define TBOX_ENGINE_CALL_TIMEOUT_MS 30000

/* One queued request line (malloc'd by the stdin thread, freed here). */
typedef struct EngineCmd {
    struct EngineCmd *next;
    char *line;
} EngineCmd;

struct TBoxEngine {
    const TBoxTdApi *api;
    TBoxEngineEmitFn emit;
    void *emit_ctx;

    char profile_dir[600];
    TBoxLog *log;
    TBoxFsxLock *lock;
    TBoxDb *db; /* index.db (spec 7.5); opened at startup, owned here */

    TBoxSession *session; /* created by auth.start, NULL until then */
    long api_id;
    char api_hash[128];   /* secret: from auth.start, never logged */
    char db_key[128];     /* secret: from auth.start, never logged */

    /* observed state (the loop owns it; used by engine.status) */
    char auth_state[32];  /* FSM name, e.g. "WAIT_QR" */
    char conn_state[64];  /* TDLib connection state name */
    int started;          /* auth.start accepted */
    int ready;            /* authorizationStateReady reached */
    int shutdown;         /* engine.shutdown seen (or input closed) */
    int input_closed;     /* stdin EOF: drain, then exit 0 */

    /* the catalog (spec 8.1 step 7, 8.2, 8.3) */
    long long my_id;      /* the account's own user id */
    long long saved_chat; /* Saved Messages chat id */
    int caption_limit;    /* TDLib option message_caption_length_max */
    TBoxUpdates *updates; /* the incremental update path */
    int op_counter;       /* source of the op_id values */
    int bootstrapped;     /* the startup catalog step ran */
    int resync_done;      /* the resync reconcile already ran */
    long long cooldown_until; /* unix seconds; 0 = no cooldown (spec 9.1) */

    /* command queue: stdin thread writes, loop thread reads */
    EngineCmd *qhead;
    EngineCmd *qtail;
    int queued;

#ifdef _WIN32
    CRITICAL_SECTION lock_q;
#else
    pthread_mutex_t lock_q;
#endif
};

/* ---- queue locking -------------------------------------------------- */

static void lock_q(TBoxEngine *e) {
#ifdef _WIN32
    EnterCriticalSection(&e->lock_q);
#else
    pthread_mutex_lock(&e->lock_q);
#endif
}

static void unlock_q(TBoxEngine *e) {
#ifdef _WIN32
    LeaveCriticalSection(&e->lock_q);
#else
    pthread_mutex_unlock(&e->lock_q);
#endif
}

/* ---- output helpers ------------------------------------------------- */

/* Emits one built line and frees it. emit failures are ignored: a broken
 * pipe is a shutdown reason, not an engine error. */
static void emit_line(TBoxEngine *e, char *line) {
    if (line == NULL) {
        return;
    }
    if (e->emit != NULL) {
        e->emit(line, e->emit_ctx);
    }
    free(line);
}

static void respond_ok(TBoxEngine *e, long long id, const cJSON *result) {
    emit_line(e, tbox_proto_response_ok(id, result));
}

static void respond_err(TBoxEngine *e, long long id, TBoxProtoError code,
                        const char *message) {
    emit_line(e, tbox_proto_response_err(id, code, message));
}

/* The FLOOD_WAIT answer (spec 6.3): the error carries the seconds to wait. */
static void respond_err_wait(TBoxEngine *e, long long id, const char *message,
                             long long retry_after) {
    emit_line(e, tbox_proto_response_err_wait(id, message, retry_after));
}

static void emit_event(TBoxEngine *e, const char *event, const cJSON *data) {
    emit_line(e, tbox_proto_event(event, data));
}

/* ---- talking to TDLib (the engine owns the one session) --------------- */

/* Forwards a line that another module built (the scan and the update path
 * both speak protocol lines). The caller keeps ownership of the line. */
static void engine_forward_line(const char *line, void *ctx) {
    TBoxEngine *e = ctx;
    if (e->emit != NULL && line != NULL) {
        e->emit(line, e->emit_ctx);
    }
}

/* ---- flood waits (spec 9.1) ------------------------------------------- */

/* True when the reply is TDLib asking to slow down. The seconds it wants are
 * in the message ("FLOOD_WAIT_42" or "retry after 42"); without one, the
 * default wait is a minute. */
static int flood_wait_seconds(const cJSON *reply, int *seconds) {
    if (reply == NULL) {
        return 0;
    }
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(reply, "code");
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(reply, "message");
    const char *m = cJSON_IsString(msg) ? cJSON_GetStringValue(msg) : "";
    int flood = 0;
    if (cJSON_IsNumber(code) && (int)code->valuedouble == 429) {
        flood = 1;
    }
    if (strstr(m, "FLOOD_WAIT") != NULL ||
        strstr(m, "Too Many Requests") != NULL) {
        flood = 1;
    }
    if (!flood) {
        return 0;
    }
    int s = 0;
    const char *p = strstr(m, "FLOOD_WAIT_");
    if (p != NULL) {
        s = atoi(p + 11);
    } else {
        p = strstr(m, "retry after ");
        if (p != NULL) {
            s = atoi(p + 11);
        }
    }
    if (s <= 0) {
        s = 60; /* tdlib's own default */
    }
    *seconds = s;
    return 1;
}

/* Pauses every request of this kind for `seconds` (+10 %) and tells the UI
 * about it (spec 9.1). */
static void engine_set_cooldown(TBoxEngine *e, int seconds, const char *reason) {
    if (seconds < 1) {
        seconds = 1;
    }
    long long now = (long long)time(NULL);
    long long until = now + seconds + seconds / 10;
    if (until > e->cooldown_until) {
        e->cooldown_until = until;
    }
    cJSON *d = cJSON_CreateObject();
    if (d == NULL) {
        return;
    }
    cJSON_AddNumberToObject(d, "seconds", (double)seconds);
    cJSON_AddStringToObject(d, "reason", reason != NULL ? reason : "rate limit");
    emit_event(e, "cooldown", d);
    cJSON_Delete(d);
    tbox_log_write(e->log, TBOX_LOG_WARN, "engine", NULL,
                   "tdlib asked to slow down; pausing requests", NULL);
}

/* Seconds left on the cooldown, 0 when there is none. */
static int engine_cooldown_left(const TBoxEngine *e) {
    long long now = (long long)time(NULL);
    if (e->cooldown_until <= now) {
        return 0;
    }
    return (int)(e->cooldown_until - now);
}

/* Sends one TDLib request and waits for its reply. Returns the parsed reply
 * (the caller frees it with cJSON_Delete) or NULL when it did not arrive.
 * A flood wait is turned into a cooldown here, so the caller only has to
 * notice that nothing came back. */
static cJSON *engine_call(TBoxEngine *e, const char *request,
                          unsigned timeout_ms) {
    if (e->session == NULL) {
        return NULL;
    }
    char *reply = tbox_session_call(e->session, request, timeout_ms);
    if (reply == NULL) {
        return NULL;
    }
    cJSON *root = cJSON_Parse(reply);
    free(reply);
    if (root == NULL) {
        return NULL;
    }
    int seconds = 0;
    if (flood_wait_seconds(root, &seconds)) {
        engine_set_cooldown(e, seconds, "Telegram asked to slow down");
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

/* Reports a TDLib failure as an engine.error event (spec 6.5: not tied to a
 * request). */
static void engine_report_td_error(TBoxEngine *e, const char *what,
                                   const cJSON *reply) {
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(reply, "code");
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(reply, "message");
    cJSON *d = cJSON_CreateObject();
    if (d == NULL) {
        return;
    }
    cJSON_AddNumberToObject(d, "code",
                            cJSON_IsNumber(code) ? code->valuedouble : 0);
    cJSON_AddStringToObject(d, "message",
                            cJSON_IsString(msg) ? cJSON_GetStringValue(msg) : what);
    emit_event(e, "engine.error", d);
    cJSON_Delete(d);
    tbox_log_write(e->log, TBOX_LOG_WARN, "engine", NULL, what, NULL);
}

/* ---- history and messages (spec 8.2, 8.3) ----------------------------- */

/* Fetches one page of Saved Messages history for the scan. Returns the
 * TDLib `messages` object as JSON, or NULL when the page did not arrive. */
static char *engine_fetch_history(void *ctx, long long from_message_id,
                                  int limit) {
    TBoxEngine *e = ctx;
    if (e->session == NULL || e->saved_chat == 0) {
        return NULL;
    }
    char request[256];
    snprintf(request, sizeof request,
             "{\"@type\":\"getChatHistory\",\"chat_id\":%lld,"
             "\"from_message_id\":%lld,\"offset\":0,\"limit\":%d,"
             "\"only_local\":false}",
             e->saved_chat, from_message_id, limit);
    cJSON *reply = engine_call(e, request, TBOX_ENGINE_CALL_TIMEOUT_MS);
    if (reply == NULL) {
        /* spec 9.1: if Telegram asked us to slow down, wait the cooldown out
         * and try once more before giving up on the page. */
        int left = engine_cooldown_left(e);
        if (left > 0) {
            TBOX_SLEEP_MS((unsigned)(left * 1000 + 500));
            reply = engine_call(e, request, TBOX_ENGINE_CALL_TIMEOUT_MS);
        }
    }
    if (reply == NULL) {
        tbox_log_write(e->log, TBOX_LOG_WARN, "sync", NULL,
                       "no answer to getChatHistory", NULL);
        return NULL;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(reply, "@type");
    if (!cJSON_IsString(type) ||
        strcmp(cJSON_GetStringValue(type), "messages") != 0) {
        engine_report_td_error(e, "getChatHistory failed", reply);
        cJSON_Delete(reply);
        return NULL;
    }
    char *page = cJSON_PrintUnformatted(reply);
    cJSON_Delete(reply);
    return page;
}

/* Fetches one message by id (the edit updates carry only an id). Returns the
 * TDLib `message` object as JSON, or NULL when it could not be read. */
static char *engine_fetch_message(void *ctx, long long message_id) {
    TBoxEngine *e = ctx;
    if (e->session == NULL) {
        return NULL;
    }
    char request[128];
    snprintf(request, sizeof request,
             "{\"@type\":\"getMessage\",\"message_id\":%lld}", message_id);
    cJSON *reply = engine_call(e, request, TBOX_ENGINE_CALL_TIMEOUT_MS);
    if (reply == NULL) {
        return NULL;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(reply, "@type");
    if (!cJSON_IsString(type) ||
        strcmp(cJSON_GetStringValue(type), "message") != 0) {
        cJSON_Delete(reply);
        return NULL;
    }
    char *msg = cJSON_PrintUnformatted(reply);
    cJSON_Delete(reply);
    return msg;
}

/* ---- the catalog ------------------------------------------------------ */

/* Runs a full scan (spec 8.2) and reports it through the protocol events.
 * The scan itself writes sync.progress, sync.done and catalog.changed. */
static void engine_run_scan(TBoxEngine *e, const char *op_id) {
    if (e->db == NULL || e->session == NULL || e->saved_chat == 0) {
        return;
    }
    TBoxScanHistory hist;
    hist.fetch = engine_fetch_history;
    hist.ctx = e;
    TBoxScanStats stats;
    memset(&stats, 0, sizeof stats);
    if (tbox_scan_full(e->db, &hist, op_id, (long long)time(NULL),
                       engine_forward_line, e, &stats) != 0) {
        /* An incomplete scan deletes nothing and leaves scan_complete alone:
         * the catalog keeps what it had. */
        tbox_log_write(e->log, TBOX_LOG_WARN, "sync", NULL,
                       "the full scan did not finish; the catalog is unchanged",
                       NULL);
    }
}

/* Runs the incremental reconcile (spec 8.3): history from the newest message
 * until the catalog is caught up, without deleting anything. */
static void engine_reconcile(TBoxEngine *e) {
    if (e->db == NULL || e->session == NULL || e->saved_chat == 0) {
        return;
    }
    TBoxScanHistory hist;
    hist.fetch = engine_fetch_history;
    hist.ctx = e;
    long long read = 0;
    if (tbox_sync_reconcile(e->db, &hist, engine_forward_line, e, &read) != 0) {
        tbox_log_write(e->log, TBOX_LOG_WARN, "sync", NULL,
                       "the reconcile did not finish", NULL);
    }
}

/* Spec 8.1 step 7: on auth.ready, open Saved Messages, read the caption
 * limit, and either scan the whole history or reconcile with it. */
static void catalog_bootstrap(TBoxEngine *e) {
    if (e->session == NULL || e->db == NULL || e->bootstrapped) {
        return;
    }
    e->bootstrapped = 1;

    e->my_id = tbox_session_my_id(e->session);
    if (e->my_id == 0) {
        cJSON *me = engine_call(e, "{\"@type\":\"getMe\"}",
                                TBOX_ENGINE_CALL_TIMEOUT_MS);
        if (me != NULL) {
            const cJSON *id = cJSON_GetObjectItemCaseSensitive(me, "id");
            if (cJSON_IsNumber(id)) {
                e->my_id = (long long)id->valuedouble;
            }
            cJSON_Delete(me);
        }
    }
    if (e->my_id == 0) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "code", 0);
        cJSON_AddStringToObject(d, "message", "the account id is not known yet");
        emit_event(e, "engine.error", d);
        cJSON_Delete(d);
        return;
    }
    {
        char id_text[32];
        snprintf(id_text, sizeof id_text, "%lld", e->my_id);
        (void)tbox_db_meta_set(e->db, TBOX_DB_META_ACCOUNT_USER_ID, id_text);
    }

    /* Saved Messages is the private chat with the account itself. */
    char request[160];
    snprintf(request, sizeof request,
             "{\"@type\":\"createPrivateChat\",\"user_id\":%lld,"
             "\"force\":true}",
             e->my_id);
    cJSON *chat = engine_call(e, request, TBOX_ENGINE_CALL_TIMEOUT_MS);
    if (chat == NULL) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "code", 0);
        cJSON_AddStringToObject(d, "message",
                                "could not open the Saved Messages chat");
        emit_event(e, "engine.error", d);
        cJSON_Delete(d);
        return;
    }
    const cJSON *chat_id = cJSON_GetObjectItemCaseSensitive(chat, "id");
    if (cJSON_IsNumber(chat_id)) {
        e->saved_chat = (long long)chat_id->valuedouble;
    }
    cJSON_Delete(chat);
    if (e->saved_chat == 0) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "code", 0);
        cJSON_AddStringToObject(d, "message",
                                "Saved Messages has no chat id yet");
        emit_event(e, "engine.error", d);
        cJSON_Delete(d);
        return;
    }
    tbox_session_set_saved_chat(e->session, e->saved_chat);

    /* The caption limit decides how long a record may be (spec 7.2). */
    cJSON *opt = engine_call(e,
                             "{\"@type\":\"getOption\","
                             "\"name\":\"message_caption_length_max\"}",
                             TBOX_ENGINE_CALL_TIMEOUT_MS);
    if (opt != NULL) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(opt, "value");
        if (cJSON_IsNumber(value)) {
            e->caption_limit = (int)value->valuedouble;
        }
        cJSON_Delete(opt);
    }

    /* From here on the incremental path knows which chat it belongs to. */
    e->updates = tbox_updates_new(e->db, e->saved_chat, engine_fetch_message, e,
                                  engine_forward_line, e);
    if (e->updates == NULL) {
        tbox_log_write(e->log, TBOX_LOG_ERROR, "sync", NULL,
                       "could not start the update path", NULL);
    }

    /* First run: scan everything. Later runs: catch up. */
    char complete[16];
    int have = tbox_db_meta_get(e->db, TBOX_DB_META_SCAN_COMPLETE, complete,
                                sizeof complete);
    if (have == 1 && strcmp(complete, "1") == 0) {
        engine_reconcile(e);
    } else {
        char op_id[17];
        snprintf(op_id, sizeof op_id, "%016llx",
                 (unsigned long long)++e->op_counter);
        engine_run_scan(e, op_id);
    }
}

/* ---- command implementations ---------------------------------------- */

/* S5: once the session is ready, make sure the local database is encrypted
 * and remember it. A brand-new profile already got the key through
 * setTdlibParameters, so only an upgraded profile needs the one-time
 * setDatabaseEncryptionKey call. Either way db_key_set is recorded so the
 * migration can never run twice. */
static void s5_after_ready(TBoxEngine *e) {
    if (e->session == NULL || e->db == NULL || e->db_key[0] == '\0') {
        return;
    }
    char v[8];
    int have = tbox_db_meta_get(e->db, TBOX_DB_META_DB_KEY_SET, v, sizeof v);
    if (have == 1 && strcmp(v, "1") == 0) {
        return; /* already encrypted by a previous run */
    }
    if (tbox_session_had_existing_db(e->session)) {
        tbox_session_encrypt_db(e->session, e->db_key);
        tbox_log_write(e->log, TBOX_LOG_INFO, "store", NULL,
                       "migrating the tdlib database to an encrypted one", NULL);
    }
    (void)tbox_db_meta_set(e->db, TBOX_DB_META_DB_KEY_SET, "1");
}

static void cmd_hello(TBoxEngine *e, const TBoxProtoRequest *req) {
    long long proto = 0;
    if (!tbox_proto_arg_long(req->args, "protocol", &proto)) {
        respond_err(e, req->id, TBOX_PROTO_ERR_BAD_ARGS,
                    "hello requires args.protocol");
        return;
    }
    if (proto != 1) {
        respond_err(e, req->id, TBOX_PROTO_ERR_PROTOCOL,
                    "unsupported protocol version");
        return;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "engine_version", TBOX_VERSION);
    cJSON_AddNumberToObject(r, "protocol", 1);
    respond_ok(e, req->id, r);
    cJSON_Delete(r);
}

static void cmd_auth_start(TBoxEngine *e, const TBoxProtoRequest *req) {
    if (e->started) {
        respond_err(e, req->id, TBOX_PROTO_ERR_BAD_ARGS,
                    "auth.start already accepted");
        return;
    }
    const char *api_id_s = tbox_proto_arg_str(req->args, "api_id");
    const char *api_hash = tbox_proto_arg_str(req->args, "api_hash");
    const char *db_key = tbox_proto_arg_str(req->args, "db_key");
    if (api_id_s == NULL || api_id_s[0] == '\0' || api_hash == NULL ||
        api_hash[0] == '\0') {
        respond_err(e, req->id, TBOX_PROTO_ERR_BAD_ARGS,
                    "auth.start requires args.api_id and args.api_hash");
        return;
    }
    char *end = NULL;
    long api_id = strtol(api_id_s, &end, 10);
    if (end == NULL || *end != '\0' || api_id <= 0) {
        respond_err(e, req->id, TBOX_PROTO_ERR_BAD_ARGS,
                    "args.api_id must be a positive integer");
        return;
    }

    e->api_id = api_id;
    snprintf(e->api_hash, sizeof e->api_hash, "%s", api_hash);
    if (db_key != NULL) {
        snprintf(e->db_key, sizeof e->db_key, "%s", db_key);
    }

    e->session = tbox_session_start(e->api, e->profile_dir, e->api_id,
                                    e->api_hash, e->db_key);
    if (e->session == NULL) {
        respond_err(e, req->id, TBOX_PROTO_ERR_INTERNAL,
                    "could not start the tdlib session");
        return;
    }
    e->started = 1;
    tbox_log_write(e->log, TBOX_LOG_INFO, "auth", NULL, "session started", NULL);
    respond_ok(e, req->id, NULL);
}

static void cmd_auth_password(TBoxEngine *e, const TBoxProtoRequest *req) {
    const char *password = tbox_proto_arg_str(req->args, "password");
    if (password == NULL) {
        respond_err(e, req->id, TBOX_PROTO_ERR_BAD_ARGS,
                    "auth.password requires args.password");
        return;
    }
    if (e->session == NULL) {
        respond_err(e, req->id, TBOX_PROTO_ERR_NOT_AUTHORIZED,
                    "auth.start has not been accepted");
        return;
    }
    tbox_session_submit_password(e->session, password);
    respond_ok(e, req->id, NULL);
}

static void cmd_auth_logout(TBoxEngine *e, const TBoxProtoRequest *req) {
    if (e->session == NULL) {
        respond_err(e, req->id, TBOX_PROTO_ERR_NOT_AUTHORIZED,
                    "auth.start has not been accepted");
        return;
    }
    tbox_session_logout(e->session);
    respond_ok(e, req->id, NULL);
}

static void cmd_engine_status(TBoxEngine *e, const TBoxProtoRequest *req) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "auth_state", e->auth_state);
    cJSON_AddStringToObject(r, "conn_state", e->conn_state);
    if (e->cooldown_until > (long long)time(NULL)) {
        cJSON_AddNumberToObject(r, "cooldown_until", (double)e->cooldown_until);
    } else {
        cJSON_AddNullToObject(r, "cooldown_until");
    }
    cJSON_AddNumberToObject(r, "pending_ops", 0); /* the op journal counts these */
    respond_ok(e, req->id, r);
    cJSON_Delete(r);
}

static void cmd_engine_shutdown(TBoxEngine *e, const TBoxProtoRequest *req) {
    respond_ok(e, req->id, NULL);
    e->shutdown = 1;
}

/* ---- catalog commands ------------------------------------------------ */

/* sync.rescan: the answer goes out first, the work follows (spec 6.2: a long
 * operation answers immediately with its op_id). */
static void cmd_sync_rescan(TBoxEngine *e, const TBoxProtoRequest *req) {
    if (!e->ready || e->session == NULL) {
        respond_err(e, req->id, TBOX_PROTO_ERR_NOT_AUTHORIZED,
                    "the session is not authorized yet");
        return;
    }
    if (e->saved_chat == 0) {
        respond_err(e, req->id, TBOX_PROTO_ERR_NOT_FOUND,
                    "Saved Messages is not open yet");
        return;
    }
    int left = engine_cooldown_left(e);
    if (left > 0) {
        /* spec 9.1: do not hammer Telegram while it asks us to slow down. */
        respond_err_wait(e, req->id, "Telegram asked us to slow down", left);
        return;
    }
    char op_id[17];
    snprintf(op_id, sizeof op_id, "%016llx",
             (unsigned long long)++e->op_counter);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "op_id", op_id);
    respond_ok(e, req->id, r);
    cJSON_Delete(r);
    engine_run_scan(e, op_id);
}

/* catalog.list: the items of one folder, in the spec 7.6 shape. */
static void cmd_catalog_list(TBoxEngine *e, const TBoxProtoRequest *req) {
    if (!e->ready) {
        respond_err(e, req->id, TBOX_PROTO_ERR_NOT_AUTHORIZED,
                    "the session is not authorized yet");
        return;
    }
    const char *folder = tbox_proto_arg_str(req->args, "folder_id");
    if (folder == NULL || folder[0] == '\0') {
        respond_err(e, req->id, TBOX_PROTO_ERR_BAD_ARGS,
                    "catalog.list requires args.folder_id");
        return;
    }
    cJSON *r = cJSON_CreateObject();
    if (r == NULL) {
        respond_err(e, req->id, TBOX_PROTO_ERR_INTERNAL, "out of memory");
        return;
    }
    if (strcmp(folder, TBOX_FOLDER_INBOX) == 0) {
        TBoxInboxRow *rows = NULL;
        size_t count = 0;
        if (tbox_query_inbox(e->db, &rows, &count) != TBOX_DB_OK) {
            cJSON_Delete(r);
            respond_err(e, req->id, TBOX_PROTO_ERR_DB, "the Inbox is unreadable");
            return;
        }
        cJSON *items = tbox_inbox_json(rows, count);
        tbox_query_free_inbox(rows, count);
        if (items == NULL) {
            cJSON_Delete(r);
            respond_err(e, req->id, TBOX_PROTO_ERR_INTERNAL, "out of memory");
            return;
        }
        cJSON_AddItemToObject(r, "items", items);
        respond_ok(e, req->id, r);
        cJSON_Delete(r);
        return;
    }
    int is_virtual = strcmp(folder, TBOX_FOLDER_ROOT) == 0 ||
                     strcmp(folder, TBOX_FOLDER_TRASH) == 0 ||
                     strcmp(folder, TBOX_FOLDER_LOST) == 0;
    if (!is_virtual) {
        /* an object id: it has to exist, or the caller gets NOT_FOUND */
        TBoxListItem probe;
        int found = 0;
        if (tbox_query_get(e->db, folder, &probe, &found) != TBOX_DB_OK) {
            cJSON_Delete(r);
            respond_err(e, req->id, TBOX_PROTO_ERR_DB, "the catalog is unreadable");
            return;
        }
        if (!found) {
            cJSON_Delete(r);
            respond_err(e, req->id, TBOX_PROTO_ERR_NOT_FOUND,
                        "no such folder");
            return;
        }
    }
    TBoxListItem *items = NULL;
    size_t count = 0;
    if (tbox_query_list(e->db, folder, &items, &count) != TBOX_DB_OK) {
        cJSON_Delete(r);
        respond_err(e, req->id, TBOX_PROTO_ERR_DB, "the catalog is unreadable");
        return;
    }
    cJSON *arr = tbox_items_json(items, count);
    tbox_query_free_items(items, count);
    if (arr == NULL) {
        cJSON_Delete(r);
        respond_err(e, req->id, TBOX_PROTO_ERR_INTERNAL, "out of memory");
        return;
    }
    cJSON_AddItemToObject(r, "items", arr);
    respond_ok(e, req->id, r);
    cJSON_Delete(r);
}

/* catalog.stat: one object and the messages that carry it. */
static void cmd_catalog_stat(TBoxEngine *e, const TBoxProtoRequest *req) {
    if (!e->ready) {
        respond_err(e, req->id, TBOX_PROTO_ERR_NOT_AUTHORIZED,
                    "the session is not authorized yet");
        return;
    }
    const char *object_id = tbox_proto_arg_str(req->args, "object_id");
    if (object_id == NULL || object_id[0] == '\0') {
        respond_err(e, req->id, TBOX_PROTO_ERR_BAD_ARGS,
                    "catalog.stat requires args.object_id");
        return;
    }
    TBoxListItem item;
    memset(&item, 0, sizeof item);
    int found = 0;
    if (tbox_query_get(e->db, object_id, &item, &found) != TBOX_DB_OK) {
        respond_err(e, req->id, TBOX_PROTO_ERR_DB, "the catalog is unreadable");
        return;
    }
    if (!found) {
        respond_err(e, req->id, TBOX_PROTO_ERR_NOT_FOUND, "no such object");
        return;
    }
    TBoxRevision *revs = NULL;
    size_t count = 0;
    if (tbox_query_revisions(e->db, object_id, &revs, &count) != TBOX_DB_OK) {
        respond_err(e, req->id, TBOX_PROTO_ERR_DB, "the catalog is unreadable");
        return;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON *one = tbox_item_json(&item);
    cJSON *arr = tbox_revisions_json(revs, count);
    tbox_query_free_revisions(revs, count);
    if (r == NULL || one == NULL || arr == NULL) {
        cJSON_Delete(r);
        cJSON_Delete(one);
        cJSON_Delete(arr);
        respond_err(e, req->id, TBOX_PROTO_ERR_INTERNAL, "out of memory");
        return;
    }
    cJSON_AddItemToObject(r, "item", one);
    cJSON_AddItemToObject(r, "revisions", arr);
    respond_ok(e, req->id, r);
    cJSON_Delete(r);
}

/* catalog.summary: counts and logical sizes per type and per top-level
 * folder. */
static void cmd_catalog_summary(TBoxEngine *e, const TBoxProtoRequest *req) {
    if (!e->ready) {
        respond_err(e, req->id, TBOX_PROTO_ERR_NOT_AUTHORIZED,
                    "the session is not authorized yet");
        return;
    }
    TBoxCatalogSummary summary;
    memset(&summary, 0, sizeof summary);
    if (tbox_query_summary(e->db, &summary) != TBOX_DB_OK) {
        respond_err(e, req->id, TBOX_PROTO_ERR_DB, "the catalog is unreadable");
        return;
    }
    cJSON *r = tbox_summary_json(&summary);
    tbox_query_free_summary(&summary);
    if (r == NULL) {
        respond_err(e, req->id, TBOX_PROTO_ERR_INTERNAL, "out of memory");
        return;
    }
    respond_ok(e, req->id, r);
    cJSON_Delete(r);
}

/* Dispatch table. Returning 0 means "not a known command". */
static void dispatch(TBoxEngine *e, const TBoxProtoRequest *req) {
    if (strcmp(req->cmd, "hello") == 0) {
        cmd_hello(e, req);
    } else if (strcmp(req->cmd, "auth.start") == 0) {
        cmd_auth_start(e, req);
    } else if (strcmp(req->cmd, "auth.password") == 0) {
        cmd_auth_password(e, req);
    } else if (strcmp(req->cmd, "auth.logout") == 0) {
        cmd_auth_logout(e, req);
    } else if (strcmp(req->cmd, "engine.status") == 0) {
        cmd_engine_status(e, req);
    } else if (strcmp(req->cmd, "engine.shutdown") == 0) {
        cmd_engine_shutdown(e, req);
    } else if (strcmp(req->cmd, "sync.rescan") == 0) {
        cmd_sync_rescan(e, req);
    } else if (strcmp(req->cmd, "catalog.list") == 0) {
        cmd_catalog_list(e, req);
    } else if (strcmp(req->cmd, "catalog.stat") == 0) {
        cmd_catalog_stat(e, req);
    } else if (strcmp(req->cmd, "catalog.summary") == 0) {
        cmd_catalog_summary(e, req);
    } else {
        respond_err(e, req->id, TBOX_PROTO_ERR_UNKNOWN_CMD,
                    "unknown command");
    }
}

/* ---- session events -> protocol events ------------------------------ */

static void handle_session_event(TBoxEngine *e, const TBoxSessionEventOut *ev) {
    cJSON *d = cJSON_CreateObject();
    switch (ev->type) {
    case TBOX_SESSION_EV_AUTH_STATE:
        snprintf(e->auth_state, sizeof e->auth_state, "%s",
                 ev->data != NULL ? ev->data : "");
        cJSON_AddStringToObject(d, "state", e->auth_state);
        emit_event(e, "auth.state", d);
        break;
    case TBOX_SESSION_EV_QR_LINK:
        /* S6: the link is never logged, only handed to the front */
        cJSON_AddStringToObject(d, "link", ev->data != NULL ? ev->data : "");
        emit_event(e, "auth.qr", d);
        break;
    case TBOX_SESSION_EV_PASSWORD_NEEDED:
        cJSON_AddStringToObject(d, "hint", ev->data != NULL ? ev->data : "");
        emit_event(e, "auth.password_needed", d);
        break;
    case TBOX_SESSION_EV_READY:
        e->ready = 1;
        e->my_id = tbox_session_my_id(e->session);
        s5_after_ready(e);
        cJSON_AddNumberToObject(d, "user_id", (double)e->my_id);
        cJSON_AddStringToObject(d, "display_name",
                                ev->data != NULL ? ev->data : "");
        cJSON_AddBoolToObject(d, "is_premium",
                              tbox_session_is_premium(e->session) != 0);
        emit_event(e, "auth.ready", d);
        /* spec 8.1 step 7: open Saved Messages and fill the catalog. */
        catalog_bootstrap(e);
        break;
    case TBOX_SESSION_EV_CONNECTION:
        snprintf(e->conn_state, sizeof e->conn_state, "%s",
                 ev->data != NULL ? ev->data : "");
        cJSON_AddStringToObject(d, "state", e->conn_state);
        emit_event(e, "conn.state", d);
        break;
    case TBOX_SESSION_EV_CLOSED:
        emit_event(e, "auth.closed", NULL);
        break;
    case TBOX_SESSION_EV_ERROR:
        cJSON_AddNumberToObject(d, "code", ev->code);
        cJSON_AddStringToObject(d, "message", ev->data != NULL ? ev->data : "");
        emit_event(e, "engine.error", d);
        break;
    case TBOX_SESSION_EV_CATALOG_CHANGED: {
        /* spec 6.5: the folders that changed, as an array ("*" = all). The
         * session hands them over as one comma-separated string. */
        cJSON *ids = cJSON_AddArrayToObject(d, "folder_ids");
        const char *list = ev->data != NULL ? ev->data : "";
        if (list[0] == '\0') {
            cJSON_AddItemToArray(ids, cJSON_CreateString("*"));
        } else {
            const char *p = list;
            while (*p != '\0') {
                const char *comma = strchr(p, ',');
                size_t n = comma != NULL ? (size_t)(comma - p) : strlen(p);
                if (n > 0) {
                    char *one = (char *)malloc(n + 1);
                    if (one != NULL) {
                        memcpy(one, p, n);
                        one[n] = '\0';
                        cJSON_AddItemToArray(ids, cJSON_CreateString(one));
                        free(one);
                    }
                }
                if (comma == NULL) {
                    break;
                }
                p = comma + 1;
            }
        }
        emit_event(e, "catalog.changed", d);
        break;
    }
    case TBOX_SESSION_EV_UPDATE:
        /* spec 8.3: one message changed in Saved Messages. */
        if (e->updates != NULL && ev->data != NULL) {
            TBoxUpdateOutcome outcome;
            if (tbox_updates_apply(e->updates, ev->data, &outcome) != 0) {
                tbox_log_write(e->log, TBOX_LOG_WARN, "sync", NULL,
                               "could not apply an update", NULL);
            } else if (outcome.needs_resync) {
                /* The message could not be read back; do not guess, ask
                 * Telegram again (spec 8.3). */
                engine_reconcile(e);
            }
        }
        break;
    default:
        break; /* transfer/sync events arrive with their phases */
    }
    cJSON_Delete(d);
}

/* Drains everything the session has queued right now. */
static void drain_session(TBoxEngine *e) {
    if (e->session == NULL) {
        return;
    }
    TBoxSessionEventOut ev;
    while (tbox_session_take_event(e->session, &ev, 0)) {
        handle_session_event(e, &ev);
        free(ev.data);
    }
}

/* ---- queue ---------------------------------------------------------- */

static EngineCmd *pop_cmd(TBoxEngine *e) {
    lock_q(e);
    EngineCmd *c = e->qhead;
    if (c != NULL) {
        e->qhead = c->next;
        if (e->qhead == NULL) {
            e->qtail = NULL;
        }
        if (e->queued > 0) {
            e->queued--;
        }
    }
    unlock_q(e);
    return c;
}

int tbox_engine_push_command(TBoxEngine *e, const char *json) {
    if (e == NULL || json == NULL) {
        return 0;
    }
    EngineCmd *c = malloc(sizeof *c);
    if (c == NULL) {
        return 0;
    }
    c->line = TBOX_STRDUP(json);
    if (c->line == NULL) {
        free(c);
        return 0;
    }
    c->next = NULL;
    lock_q(e);
    if (e->shutdown) {
        unlock_q(e);
        free(c->line);
        free(c);
        return 0;
    }
    if (e->qtail != NULL) {
        e->qtail->next = c;
    } else {
        e->qhead = c;
    }
    e->qtail = c;
    e->queued++;
    unlock_q(e);
    return 1;
}

void tbox_engine_signal_input_closed(TBoxEngine *e) {
    if (e == NULL) {
        return;
    }
    lock_q(e);
    e->input_closed = 1;
    unlock_q(e);
}

/* ---- loop ----------------------------------------------------------- */

static void run_one_command(TBoxEngine *e, const char *line) {
    TBoxProtoRequest req;
    tbox_proto_parse(line, strlen(line), &req);
    if (req.err != TBOX_PROTO_OK) {
        /* no id means we cannot answer with an id; the spec shape needs
         * one, so malformed lines are dropped once logged. */
        if (req.has_id) {
            respond_err(e, req.id, req.err,
                        "malformed request");
        } else {
            tbox_log_write(e->log, TBOX_LOG_WARN, "engine", NULL,
                           "dropped a malformed request line", NULL);
        }
        tbox_proto_request_clear(&req);
        return;
    }
    tbox_log_write(e->log, TBOX_LOG_DEBUG, "engine", NULL, req.cmd, NULL);
    dispatch(e, &req);
    tbox_proto_request_clear(&req);
}

static void shutdown_session(TBoxEngine *e) {
    if (e->session == NULL) {
        return;
    }
    /* spec 9.4: ask TDLib to close, then wait (bounded, max 10 s) for
     * authorizationStateClosed. If it already arrived, there is nothing
     * to wait for. */
    tbox_session_request_close(e->session);
    if (strcmp(e->auth_state, "CLOSED") != 0) {
        unsigned waited = 0;
        TBoxSessionEventOut ev;
        while (waited < 10000) {
            if (tbox_session_take_event(e->session, &ev, 100)) {
                int closed = (ev.type == TBOX_SESSION_EV_CLOSED) ||
                             (ev.type == TBOX_SESSION_EV_AUTH_STATE &&
                              ev.data != NULL && strcmp(ev.data, "CLOSED") == 0);
                free(ev.data);
                if (closed) {
                    break;
                }
            }
            waited += 100;
        }
    }
    tbox_session_close(e->session);
    e->session = NULL;
}

int tbox_engine_run(TBoxEngine *e) {
    if (e == NULL) {
        return 1;
    }
    for (;;) {
        EngineCmd *c = pop_cmd(e);
        if (c != NULL) {
            run_one_command(e, c->line);
            free(c->line);
            free(c);
            continue; /* drain the queue before sleeping */
        }
        drain_session(e);
        if (e->session != NULL && tbox_session_resync_needed(e->session) &&
            !e->resync_done) {
            e->resync_done = 1;
            /* spec 6.6: the front reloads what it shows, and the engine
             * reconciles with Telegram instead of guessing. */
            cJSON *d = cJSON_CreateObject();
            cJSON *arr = cJSON_CreateArray();
            cJSON_AddItemToArray(arr, cJSON_CreateString("*"));
            cJSON_AddItemToObject(d, "folder_ids", arr);
            emit_event(e, "catalog.changed", d);
            cJSON_Delete(d);
            engine_reconcile(e);
        }
        if (e->shutdown || e->input_closed) {
            break;
        }
        TBOX_SLEEP_MS(TBOX_ENGINE_POLL_MS);
    }
    shutdown_session(e);
    tbox_log_write(e->log, TBOX_LOG_INFO, "engine", NULL, "shutdown", NULL);
    return 0;
}

/* ---- lifecycle ------------------------------------------------------ */

TBoxEngine *tbox_engine_new(const TBoxTdApi *api, const TBoxEngineConfig *cfg,
                            TBoxEngineEmitFn emit, void *emit_ctx,
                            int *err) {
    if (err != NULL) {
        *err = 1;
    }
    if (api == NULL || cfg == NULL || cfg->profile_dir == NULL ||
        cfg->profile_dir[0] == '\0') {
        return NULL;
    }
    TBoxEngine *e = calloc(1, sizeof *e);
    if (e == NULL) {
        return NULL;
    }
    e->api = api;
    e->emit = emit;
    e->emit_ctx = emit_ctx;
    snprintf(e->profile_dir, sizeof e->profile_dir, "%s", cfg->profile_dir);
    snprintf(e->auth_state, sizeof e->auth_state, "START");
#ifdef _WIN32
    InitializeCriticalSection(&e->lock_q);
#else
    pthread_mutex_init(&e->lock_q, NULL);
#endif

    /* spec 8.1 step 2: logs/ and the log. A failed log open is not fatal. */
    if (tbox_log_open(e->profile_dir, 0, 0, &e->log) != TBOX_LOG_OK) {
        e->log = NULL;
    }

    /* spec 8.1 step 3: one engine per profile. */
    TBoxFsxStatus ls = tbox_fsx_lock(e->profile_dir, &e->lock);
    if (ls == TBOX_FSX_E_BUSY) {
        if (err != NULL) {
            *err = 2;
        }
        tbox_engine_free(e);
        return NULL;
    }
    if (ls != TBOX_FSX_OK) {
        if (err != NULL) {
            *err = 1;
        }
        tbox_engine_free(e);
        return NULL;
    }

    /* spec 8.1 step 4: open index.db (migrations run inside). A failure is
     * exit code 4 (spec 6.1). */
    TBoxDbStatus ds = tbox_db_open(e->profile_dir, &e->db);
    if (ds != TBOX_DB_OK) {
        tbox_log_write(e->log, TBOX_LOG_ERROR, "store", NULL,
                       "could not open index.db", NULL);
        if (err != NULL) {
            *err = TBOX_SERVE_DB_ERROR;
        }
        tbox_engine_free(e);
        return NULL;
    }

    if (err != NULL) {
        *err = 0;
    }
    return e;
}

void tbox_engine_free(TBoxEngine *e) {
    if (e == NULL) {
        return;
    }
    if (e->updates != NULL) {
        tbox_updates_free(e->updates);
        e->updates = NULL;
    }
    if (e->session != NULL) {
        tbox_session_close(e->session);
        e->session = NULL;
    }
    tbox_db_close(e->db);
    EngineCmd *c = e->qhead;
    while (c != NULL) {
        EngineCmd *next = c->next;
        free(c->line);
        free(c);
        c = next;
    }
    tbox_fsx_unlock(e->lock);
    tbox_log_close(e->log);
#ifdef _WIN32
    DeleteCriticalSection(&e->lock_q);
#else
    pthread_mutex_destroy(&e->lock_q);
#endif
    free(e);
}
