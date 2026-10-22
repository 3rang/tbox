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
#include "store/catalog.h"
#include "store/db.h"
#include "store/journal.h"
#include "store/query.h"
#include "sync/scan.h"
#include "sync/updates.h"
#include "util/fsx.h"
#include "util/log.h"
#include "xfer/ops.h"
#include "xfer/upload.h"

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

    /* the upload in flight (spec 8.5: one upload at a time) */
    int upload_busy;           /* a file is on its way up */
    int upload_pending;        /* the loop has an upload to pick up */
    long long upload_next_at;  /* the earliest time a waiting op may run */
    char upload_op[TBOX_OP_HEX_LEN + 1];
    char upload_file[TBOX_MSGID_MAX_BYTES + 1];
    char *upload_path;         /* the file being sent (heap) */
    long long upload_size;
    long long upload_mtime;

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

/* Answers "has this operation already gone out?" - the record of a file that
 * was sent carries the id of the operation that sent it (spec 7.2), so the
 * catalog knows whether the message is there. */
static int engine_op_sent(void *ctx, const char *op_id) {
    TBoxEngine *e = (TBoxEngine *)ctx;
    if (e->db == NULL) {
        return 0;
    }
    return tbox_cat_find_op_message(e->db, op_id, NULL);
}

/* Spec 8.4: the operations that were still open when the engine stopped are
 * looked at when it starts again. An upload that was running or sent may
 * already be in Saved Messages; if it is there the operation is done and is
 * not sent a second time, and if it is not, it goes back into the queue.
 *
 * This runs after the catalog has been brought up to date, so the message of
 * an operation that went out just before the crash is found. */
static void engine_recover_ops(TBoxEngine *e) {
    if (e->db == NULL) {
        return;
    }
    TBoxOpRecoveryReport report;
    memset(&report, 0, sizeof report);
    if (tbox_journal_recover(e->db, engine_op_sent, e, (long long)time(NULL),
                              &report) != TBOX_DB_OK) {
        tbox_log_write(e->log, TBOX_LOG_WARN, "xfer", NULL,
                       "the operations of the last run could not be looked at",
                       NULL);
        return;
    }
    if (report.checked == 0) {
        return;
    }
    char detail[160];
    snprintf(detail, sizeof detail,
             "%d open operation(s): %d already done, %d back in the queue",
             report.checked, report.completed, report.requeued);
    tbox_log_write(e->log, TBOX_LOG_INFO, "xfer", NULL,
                   "the operations of the last run were looked at", detail);
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

    /* The catalog is up to date, so the operations of the last run can be
     * looked at (spec 8.4). */
    engine_recover_ops(e);
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
    int pending = 0;
    if (e->db != NULL) {
        (void)tbox_journal_count_active(e->db, &pending);
    }
    cJSON_AddNumberToObject(r, "pending_ops", (double)pending);
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

/* ---- uploads (spec 8.5) ----------------------------------------------- */

/* The upload's way of talking to TDLib: one request, one reply. */
static cJSON *engine_upload_call(void *ctx, const char *request) {
    TBoxEngine *e = ctx;
    return engine_call(e, request, TBOX_ENGINE_CALL_TIMEOUT_MS);
}

/* The world the upload module works in. */
static void engine_upload_env(TBoxEngine *e, TBoxUploadEnv *env) {
    memset(env, 0, sizeof *env);
    env->db = e->db;
    env->call = engine_upload_call;
    env->call_ctx = e;
    env->chat_id = e->saved_chat;
    env->caption_limit = (unsigned)e->caption_limit;
    env->is_premium = e->session != NULL && tbox_session_is_premium(e->session);
    env->now = (long long)time(NULL);
}

/* The error the protocol shows for a failed upload. */
static TBoxProtoError engine_upload_error(TBoxUploadStatus status) {
    switch (status) {
    case TBOX_UPLOAD_E_ARGS:
        return TBOX_PROTO_ERR_BAD_ARGS;
    case TBOX_UPLOAD_E_FILE:
    case TBOX_UPLOAD_E_FOLDER:
        return TBOX_PROTO_ERR_NOT_FOUND;
    case TBOX_UPLOAD_E_TOO_LARGE:
        return TBOX_PROTO_ERR_FILE_TOO_LARGE;
    case TBOX_UPLOAD_E_NAME:
    case TBOX_UPLOAD_E_CAPTION:
        return TBOX_PROTO_ERR_NAME_INVALID;
    case TBOX_UPLOAD_E_EXISTS:
        return TBOX_PROTO_ERR_NAME_EXISTS;
    case TBOX_UPLOAD_E_IO:
        return TBOX_PROTO_ERR_IO;
    case TBOX_UPLOAD_E_DB:
        return TBOX_PROTO_ERR_DB;
    case TBOX_UPLOAD_E_TD:
        return TBOX_PROTO_ERR_TD;
    default:
        return TBOX_PROTO_ERR_INTERNAL;
    }
}

/* Tells the front that an operation did not work out (spec 6.5). */
static void engine_emit_transfer_failed(TBoxEngine *e, const char *op_id,
                                         TBoxProtoError code,
                                         const char *message) {
    cJSON *d = cJSON_CreateObject();
    if (d == NULL) {
        return;
    }
    cJSON_AddStringToObject(d, "op_id", op_id != NULL ? op_id : "");
    cJSON *err = cJSON_AddObjectToObject(d, "error");
    cJSON_AddStringToObject(err, "code", tbox_proto_error_code(code));
    cJSON_AddStringToObject(err, "message", message != NULL ? message : "");
    emit_event(e, "transfer.failed", d);
    cJSON_Delete(d);
}

/* An upload that did not work out. The operation waits and tries again, or
 * gives up (spec 8.4: 5 s, 30 s, 2 min, 10 min). */
static void engine_fail_upload(TBoxEngine *e, const char *op_id,
                                TBoxProtoError code, const char *message) {
    if (e->db == NULL || op_id == NULL || op_id[0] == '\0') {
        return;
    }
    long long now = (long long)time(NULL);

    TBoxOpRow row;
    memset(&row, 0, sizeof row);
    int found = 0;
    (void)tbox_journal_get(e->db, op_id, &row, &found);
    TBoxOpState from = TBOX_OP_QUEUED;
    if (found) {
        (void)tbox_op_state_parse(row.state, &from);
    }
    tbox_journal_free_row(&row);

    int attempts = 0;
    (void)tbox_journal_bump_attempts(e->db, op_id, now, &attempts);
    int wait = tbox_op_backoff_seconds(attempts);
    const char *state;
    if (wait < 0) {
        state = "FAILED"; /* it has used up its tries */
    } else if (from == TBOX_OP_SENT) {
        /* Telegram had it and then said no: it never made it out, so it goes
         * back into the queue. */
        state = "QUEUED";
    } else if (from == TBOX_OP_RUNNING) {
        state = "RETRY_WAIT";
    } else {
        state = "FAILED"; /* it never got off the ground */
    }
    long long next_try_at = wait < 0 ? 0 : now + wait;
    (void)tbox_journal_set_state(e->db, op_id, state,
                                 tbox_proto_error_code(code), next_try_at, now);
    engine_emit_transfer_failed(e, op_id, code, message);
    tbox_log_write(e->log, TBOX_LOG_WARN, "xfer", op_id, message, NULL);
    if (strcmp(e->upload_op, op_id) == 0) {
        e->upload_busy = 0;
    }
    e->upload_pending = 1;
    e->upload_next_at = wait < 0 ? 0 : next_try_at;
}

/* The object an operation is about, out of the arguments it kept. */
static void engine_op_object_id(TBoxEngine *e, const char *op_id, char *out,
                                size_t cap) {
    if (out == NULL || cap == 0) {
        return;
    }
    out[0] = '\0';
    TBoxOpRow row;
    memset(&row, 0, sizeof row);
    int found = 0;
    if (tbox_journal_get(e->db, op_id, &row, &found) != TBOX_DB_OK || !found) {
        tbox_journal_free_row(&row);
        return;
    }
    TBoxUploadArgs args;
    if (tbox_upload_args_parse(row.args_json, &args)) {
        snprintf(out, cap, "%s", args.object_id != NULL ? args.object_id : "");
        tbox_upload_args_free(&args);
    }
    tbox_journal_free_row(&row);
}

/* The operation that is waiting for this message id. */
static int engine_find_op_by_temp(TBoxEngine *e, const char *msg_id,
                                  char *op_id, size_t cap) {
    if (e->db == NULL || msg_id == NULL || msg_id[0] == '\0') {
        return 0;
    }
    TBoxOpRow *rows = NULL;
    size_t count = 0;
    if (tbox_journal_list_active(e->db, &rows, &count) != TBOX_DB_OK) {
        return 0;
    }
    int found = 0;
    for (size_t i = 0; i < count && !found; i++) {
        if (rows[i].temp_msg_id[0] != '\0' &&
            strcmp(rows[i].temp_msg_id, msg_id) == 0) {
            snprintf(op_id, cap, "%s", rows[i].op_id);
            found = 1;
        }
    }
    tbox_journal_free_rows(rows, count);
    return found;
}

/* Runs one upload: the checks, the hash and the message (spec 8.5). */
static void engine_start_upload(TBoxEngine *e, const char *op_id,
                                 const char *args_json) {
    long long now = (long long)time(NULL);
    TBoxUploadArgs args;
    if (!tbox_upload_args_parse(args_json, &args)) {
        engine_fail_upload(e, op_id, TBOX_PROTO_ERR_INTERNAL,
                           "the operation has no usable arguments");
        return;
    }
    /* the operation is on its way now: from here on a failure waits and
     * tries again (spec 8.4) */
    if (tbox_journal_set_state(e->db, op_id, "RUNNING", NULL, 0, now) !=
        TBOX_DB_OK) {
        tbox_upload_args_free(&args);
        engine_fail_upload(e, op_id, TBOX_PROTO_ERR_INTERNAL,
                           "the operation could not be written down");
        return;
    }

    TBoxUploadEnv env;
    engine_upload_env(e, &env);
    TBoxUploadRequest ureq;
    memset(&ureq, 0, sizeof ureq);
    ureq.local_path = args.local_path;
    ureq.folder_id = args.folder_id;
    ureq.name = args.name;

    char err[256];
    err[0] = '\0';
    TBoxUploadChecked checked;
    TBoxUploadStatus st =
        tbox_upload_check(&env, &ureq, &checked, err, sizeof err);
    if (st == TBOX_UPLOAD_OK) {
        TBoxUploadPlan plan;
        st = tbox_upload_send(&env, op_id, args.object_id, &checked,
                              args.local_path, &plan, err, sizeof err);
        if (st == TBOX_UPLOAD_OK) {
            /* remember the flight: the progress and the end of it come with
             * the id of the file */
            e->upload_busy = 1;
            snprintf(e->upload_op, sizeof e->upload_op, "%s", plan.op_id);
            snprintf(e->upload_file, sizeof e->upload_file, "%s", plan.file_id);
            free(e->upload_path);
            e->upload_path = TBOX_STRDUP(args.local_path);
            e->upload_size = plan.size;
            e->upload_mtime = plan.mtime;
            tbox_upload_args_free(&args);
            tbox_log_write(e->log, TBOX_LOG_INFO, "xfer", op_id,
                           "the file is on its way", NULL);
            return;
        }
    }
    tbox_upload_args_free(&args);
    engine_fail_upload(e, op_id, engine_upload_error(st), err);
}

/* Starts the upload that is next in the queue (spec 8.5: one at a time).
 * The queue is the journal itself, so an operation that was left over from
 * a crash is picked up as well. */
static void engine_run_uploads(TBoxEngine *e) {
    if (e->db == NULL || e->session == NULL || e->saved_chat == 0) {
        return;
    }
    if (!e->upload_pending) {
        return;
    }
    long long now = (long long)time(NULL);
    if (now < e->upload_next_at) {
        return;
    }
    e->upload_pending = 0;
    if (e->upload_busy) {
        return; /* one upload at a time */
    }
    if (engine_cooldown_left(e) > 0) {
        /* spec 9.1: Telegram asked us to slow down, and a flood wait
         * overrides the retry delay. */
        e->upload_pending = 1;
        return;
    }

    TBoxOpRow *rows = NULL;
    size_t count = 0;
    if (tbox_journal_list_active(e->db, &rows, &count) != TBOX_DB_OK) {
        return;
    }
    char op_id[TBOX_OP_HEX_LEN + 1];
    char *args_json = NULL;
    long long next_at = 0;
    op_id[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        if (strcmp(rows[i].kind, TBOX_OP_KIND_UPLOAD) != 0) {
            continue;
        }
        if (strcmp(rows[i].state, "QUEUED") != 0 &&
            strcmp(rows[i].state, "RETRY_WAIT") != 0) {
            continue;
        }
        if (rows[i].next_try_at > now) {
            if (next_at == 0 || rows[i].next_try_at < next_at) {
                next_at = rows[i].next_try_at;
            }
            continue;
        }
        snprintf(op_id, sizeof op_id, "%s", rows[i].op_id);
        if (rows[i].args_json != NULL) {
            args_json = TBOX_STRDUP(rows[i].args_json);
        }
        break;
    }
    tbox_journal_free_rows(rows, count);

    if (op_id[0] == '\0') {
        /* nothing to do now: wake up when the earliest waiting op is due */
        e->upload_next_at = next_at;
        return;
    }
    engine_start_upload(e, op_id, args_json);
    free(args_json);
}

/* updateFile: the progress of the file that is going up (spec 8.5 step 6). */
static void engine_upload_progress(TBoxEngine *e, const cJSON *update) {
    if (!e->upload_busy || e->upload_path == NULL) {
        return;
    }
    const cJSON *file = cJSON_GetObjectItemCaseSensitive(update, "file");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(file, "id");
    if (!cJSON_IsNumber(id)) {
        return;
    }
    char file_id[TBOX_MSGID_MAX_BYTES + 1];
    snprintf(file_id, sizeof file_id, "%lld", (long long)id->valuedouble);
    if (strcmp(file_id, e->upload_file) != 0) {
        return; /* some other file */
    }

    /* spec 8.5 step 9: the file must not change while it goes up. TDLib
     * cannot stop an upload that is already running, so the operation is
     * marked for the user and the message it leaves behind is theirs to
     * delete. */
    TBoxFileInfo info;
    if (tbox_fsx_file_info(e->upload_path, &info) == TBOX_FSX_OK &&
        info.exists &&
        (info.size != e->upload_size || info.mtime != e->upload_mtime)) {
        long long now = (long long)time(NULL);
        (void)tbox_journal_set_state(e->db, e->upload_op, "NEEDS_ATTENTION",
                                     "CANCELLED", 0, now);
        engine_emit_transfer_failed(e, e->upload_op, TBOX_PROTO_ERR_CANCELLED,
                                    "the file changed while it was being sent");
        tbox_log_write(e->log, TBOX_LOG_WARN, "xfer", e->upload_op,
                       "the file changed while it was being sent", NULL);
        e->upload_pending = 1;
        return;
    }

    const cJSON *size = cJSON_GetObjectItemCaseSensitive(file, "size");
    const cJSON *expected =
        cJSON_GetObjectItemCaseSensitive(file, "expected_size");
    long long done = cJSON_IsNumber(size) ? (long long)size->valuedouble : 0;
    long long total = cJSON_IsNumber(expected)
                          ? (long long)expected->valuedouble
                          : e->upload_size;
    cJSON *d = cJSON_CreateObject();
    if (d == NULL) {
        return;
    }
    cJSON_AddStringToObject(d, "op_id", e->upload_op);
    cJSON_AddNumberToObject(d, "done", (double)done);
    cJSON_AddNumberToObject(d, "total", (double)total);
    emit_event(e, "transfer.progress", d);
    cJSON_Delete(d);
}

/* updateMessageSendSucceeded: the file is in Saved Messages (spec 8.5
 * step 7). The operation is done and the front is told. */
static void engine_upload_succeeded(TBoxEngine *e, const cJSON *update) {
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(update, "message");
    const cJSON *new_id = cJSON_GetObjectItemCaseSensitive(msg, "id");
    const cJSON *old_id =
        cJSON_GetObjectItemCaseSensitive(update, "old_message_id");
    if (!cJSON_IsNumber(old_id)) {
        return;
    }
    char old_buf[TBOX_MSGID_MAX_BYTES + 1];
    char new_buf[TBOX_MSGID_MAX_BYTES + 1];
    snprintf(old_buf, sizeof old_buf, "%lld", (long long)old_id->valuedouble);
    new_buf[0] = '\0';
    if (cJSON_IsNumber(new_id)) {
        snprintf(new_buf, sizeof new_buf, "%lld", (long long)new_id->valuedouble);
    }
    char op_id[TBOX_OP_HEX_LEN + 1];
    op_id[0] = '\0';
    if (!engine_find_op_by_temp(e, old_buf, op_id, sizeof op_id) &&
        !engine_find_op_by_temp(e, new_buf, op_id, sizeof op_id)) {
        /* The catalog already moved the temporary id to the real one
         * (spec 8.3), so the operation is looked for under both. */
        return;
    }
    long long now = (long long)time(NULL);
    if (tbox_journal_set_state(e->db, op_id, "DONE", NULL, 0, now) ==
        TBOX_DB_OK) {
        char object_id[TBOX_ID_HEX_LEN + 1];
        engine_op_object_id(e, op_id, object_id, sizeof object_id);
        cJSON *d = cJSON_CreateObject();
        if (d != NULL) {
            cJSON_AddStringToObject(d, "op_id", op_id);
            cJSON_AddStringToObject(d, "object_id", object_id);
            emit_event(e, "transfer.done", d);
            cJSON_Delete(d);
        }
    }
    if (strcmp(e->upload_op, op_id) == 0) {
        e->upload_busy = 0;
    }
    e->upload_pending = 1;
}

/* updateMessageSendFailed: the message did not make it (spec 8.5). */
static void engine_upload_failed(TBoxEngine *e, const cJSON *update) {
    const cJSON *mid = cJSON_GetObjectItemCaseSensitive(update, "message_id");
    if (!cJSON_IsNumber(mid)) {
        return;
    }
    char buf[TBOX_MSGID_MAX_BYTES + 1];
    snprintf(buf, sizeof buf, "%lld", (long long)mid->valuedouble);
    char op_id[TBOX_OP_HEX_LEN + 1];
    if (!engine_find_op_by_temp(e, buf, op_id, sizeof op_id)) {
        return;
    }
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(update, "error_code");
    char message[160];
    snprintf(message, sizeof message, "the message could not be sent (%d)",
             cJSON_IsNumber(code) ? (int)code->valuedouble : 0);
    engine_fail_upload(e, op_id, TBOX_PROTO_ERR_TD, message);
}

/* The updates that are about a transfer (spec 8.5). */
static void engine_transfer_update(TBoxEngine *e, const char *update_json) {
    if (update_json == NULL) {
        return;
    }
    cJSON *update = cJSON_Parse(update_json);
    if (update == NULL || !cJSON_IsObject(update)) {
        cJSON_Delete(update);
        return;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(update, "@type");
    const char *t = cJSON_IsString(type) ? cJSON_GetStringValue(type) : "";
    if (strcmp(t, "updateFile") == 0) {
        engine_upload_progress(e, update);
    } else if (strcmp(t, "updateMessageSendSucceeded") == 0) {
        engine_upload_succeeded(e, update);
    } else if (strcmp(t, "updateMessageSendFailed") == 0) {
        engine_upload_failed(e, update);
    }
    cJSON_Delete(update);
}

/* file.upload: the file goes into Saved Messages (spec 8.5). The answer
 * goes out first, with the operation id; the upload follows, one at a time
 * (spec 6.2). */
static void cmd_file_upload(TBoxEngine *e, const TBoxProtoRequest *req) {
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
    const char *local_path = tbox_proto_arg_str(req->args, "local_path");
    const char *folder_id = tbox_proto_arg_str(req->args, "folder_id");
    const char *name = tbox_proto_arg_str(req->args, "name");
    const char *on_exists = tbox_proto_arg_str(req->args, "on_exists");
    if (local_path == NULL || local_path[0] == '\0' || folder_id == NULL ||
        folder_id[0] == '\0') {
        respond_err(e, req->id, TBOX_PROTO_ERR_BAD_ARGS,
                    "file.upload requires args.local_path and args.folder_id");
        return;
    }
    if (on_exists != NULL && on_exists[0] != '\0' &&
        strcmp(on_exists, "error") != 0) {
        /* the other mode - a new revision of an object that is already
         * there - is not here yet; saying so beats pretending */
        respond_err(e, req->id, TBOX_PROTO_ERR_BAD_ARGS,
                    "on_exists must be \"error\"");
        return;
    }
    int left = engine_cooldown_left(e);
    if (left > 0) {
        respond_err_wait(e, req->id, "Telegram asked us to slow down", left);
        return;
    }

    TBoxUploadEnv env;
    engine_upload_env(e, &env);
    TBoxUploadRequest ureq;
    memset(&ureq, 0, sizeof ureq);
    ureq.local_path = local_path;
    ureq.folder_id = folder_id;
    ureq.name = name;

    char err[256];
    err[0] = '\0';
    TBoxUploadChecked checked;
    TBoxUploadStatus st =
        tbox_upload_check(&env, &ureq, &checked, err, sizeof err);
    if (st != TBOX_UPLOAD_OK) {
        respond_err(e, req->id, engine_upload_error(st), err);
        return;
    }
    TBoxUploadOp op;
    st = tbox_upload_queue(&env, &ureq, &checked, &op, err, sizeof err);
    if (st != TBOX_UPLOAD_OK) {
        respond_err(e, req->id, engine_upload_error(st), err);
        return;
    }
    cJSON *r = cJSON_CreateObject();
    if (r == NULL) {
        respond_err(e, req->id, TBOX_PROTO_ERR_INTERNAL, "out of memory");
        return;
    }
    cJSON_AddStringToObject(r, "op_id", op.op_id);
    respond_ok(e, req->id, r);
    cJSON_Delete(r);
    /* the engine sends it when it is its turn */
    e->upload_pending = 1;
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
    } else if (strcmp(req->cmd, "file.upload") == 0) {
        cmd_file_upload(e, req);
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
        /* the transfers: the progress and the end of an upload (spec 8.5) */
        engine_transfer_update(e, ev->data);
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
    /* Detach before closing: the session belongs to this thread from here
     * on, so nobody else can close it a second time. */
    TBoxSession *s = e->session;
    e->session = NULL;
    /* spec 9.4: ask TDLib to close, then wait (bounded, max 10 s) for
     * authorizationStateClosed. If it already arrived, there is nothing
     * to wait for. */
    tbox_session_request_close(s);
    if (strcmp(e->auth_state, "CLOSED") != 0) {
        unsigned waited = 0;
        TBoxSessionEventOut ev;
        while (waited < 10000) {
            if (tbox_session_take_event(s, &ev, 100)) {
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
    tbox_session_close(s);
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
        /* spec 8.5: the uploads go one at a time. */
        engine_run_uploads(e);
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
        /* The run loop closes the session when it stops (shutdown_session).
         * A session that is still here means the loop never ran, so this is
         * the only place that can close it. */
        tbox_session_close(e->session);
        e->session = NULL;
    }
    tbox_db_close(e->db);
    free(e->upload_path);
    e->upload_path = NULL;
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
