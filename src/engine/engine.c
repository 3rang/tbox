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

#include "cmd/cmd.h" /* TBOX_VERSION */
#include "engine/protocol.h"
#include "service/tbox_session.h"
#include "store/db.h"
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

static void emit_event(TBoxEngine *e, const char *event, const cJSON *data) {
    emit_line(e, tbox_proto_event(event, data));
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
    cJSON_AddNullToObject(r, "cooldown_until"); /* set once transfers land */
    cJSON_AddNumberToObject(r, "pending_ops", 0); /* the op journal counts these */
    respond_ok(e, req->id, r);
    cJSON_Delete(r);
}

static void cmd_engine_shutdown(TBoxEngine *e, const TBoxProtoRequest *req) {
    respond_ok(e, req->id, NULL);
    e->shutdown = 1;
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
        s5_after_ready(e);
        cJSON_AddStringToObject(d, "display_name",
                                ev->data != NULL ? ev->data : "");
        emit_event(e, "auth.ready", d);
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
    case TBOX_SESSION_EV_CATALOG_CHANGED:
        cJSON_AddStringToObject(d, "folder", ev->data != NULL ? ev->data : "");
        emit_event(e, "catalog.changed", d);
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
        if (e->session != NULL && tbox_session_resync_needed(e->session)) {
            /* spec 6.6: the front reloads everything; the reconcile itself
             * runs once the catalog exists. */
            cJSON *d = cJSON_CreateObject();
            cJSON *arr = cJSON_CreateArray();
            cJSON_AddItemToArray(arr, cJSON_CreateString("*"));
            cJSON_AddItemToObject(d, "folder_ids", arr);
            emit_event(e, "catalog.changed", d);
            cJSON_Delete(d);
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
