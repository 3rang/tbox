/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Tarang Patel
 *
 * tdhandler.c - the `tbox auth` QR login flow, ported from the proven
 * demo/qr_login.c loop. One small function per concern:
 *
 *   send_req / request_qr        - JSON plumbing to td_send
 *   type_of / extra_is           - update inspection
 *   prompt_and_send_password     - 2FA (uses util/term masked input)
 *   on_auth_state                - authorizationState machine steps
 *   on_user / on_error           - getMe result + TDLib errors
 *   tbox_td_auth_run             - the td_receive loop (worker thread)
 */

#include "tdhandler.h"
#include "tdparams.h"

#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

#include "util/qrprint.h"
#include "util/term.h"

#define RECV_TIMEOUT 1.0 /* seconds each td_receive() poll waits */

/* how long to wait for TDLib to acknowledge close after a cancel */
#define CLOSE_DRAIN_TICKS 2

typedef struct
{
    int client_id;
    int qr_sent;         /* requestQrCodeAuthentication sent once */
    int getme_sent;      /* getMe sent once */
    int close_sent;      /* close (normal or cancelled) sent once */
    int done;            /* authorizationStateClosed seen */
    int ready;           /* authorizationStateReady seen */
    int fatal;           /* TDLib error that ends the run */
    char prev_link[512]; /* last QR link printed (QR rotates) */
    int vt;              /* ANSI console mode usable */

} auth_ctx_t;

/* ---- small JSON helpers (from the demo, unchanged) ------------------------ */

static const char *type_of(const cJSON *obj)
{
    const cJSON *t;

    if (obj == NULL)
        return "";
    t = cJSON_GetObjectItem(obj, "@type");
    return cJSON_IsString(t) ? t->valuestring : "";
}

static int extra_is(const cJSON *obj, const char *want)
{
    const cJSON *e = cJSON_GetObjectItem(obj, "@extra");

    return cJSON_IsString(e) && strcmp(e->valuestring, want) == 0;
}

static void send_req(int client_id, cJSON *req)
{
    char *json = cJSON_PrintUnformatted(req);

    if (json != NULL) {
        td_send(client_id, json);
        cJSON_free(json);
    }
    cJSON_Delete(req);
}

static void request_qr(int client_id)
{
    cJSON *req = cJSON_CreateObject();

    cJSON_AddStringToObject(req, "@type", "requestQrCodeAuthentication");
    cJSON_AddItemToObject(req, "other_user_ids", cJSON_CreateArray());
    send_req(client_id, req);
}

static void send_close(auth_ctx_t *ctx)
{
    cJSON *close = cJSON_CreateObject();

    ctx->close_sent = 1;
    cJSON_AddStringToObject(close, "@type", "close");
    send_req(ctx->client_id, close);
}

/* ---- 2FA ------------------------------------------------------------- */

static void prompt_and_send_password(auth_ctx_t *ctx)
{
    char pw[256];
    int len;

    printf("  Two-step verification is enabled on this account.\n"
           "  Enter your 2FA password: ");
    fflush(stdout);

    len = tbox_term_read_masked_line(pw, sizeof pw);

    if (len < 0) {
        printf("  Cancelled.\n");
        send_close(ctx);
        return;
    }

    {
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "@type", "checkAuthenticationPassword");
        cJSON_AddStringToObject(req, "password", pw);
        cJSON_AddStringToObject(req, "@extra", "pwd");
        send_req(ctx->client_id, req);
    }
    memset(pw, 0, sizeof pw);
}

/* ---- update handlers -------------------------------------------------- */

/* authorizationStateWaitOtherDeviceConfirmation -> render the QR link. */
static void show_qr_if_new(auth_ctx_t *ctx, const char *link)
{
    if (link != NULL && link[0] != '\0' && strcmp(link, ctx->prev_link) != 0) {
        snprintf(ctx->prev_link, sizeof ctx->prev_link, "%s", link);
        tbox_qr_print(link, ctx->vt);
    }
}

/* updateAuthorizationState -> the state machine. */
static void on_auth_state(auth_ctx_t *ctx, const cJSON *upd)
{
    const cJSON *state = cJSON_GetObjectItem(upd, "authorization_state");
    const char *st = type_of(state);

    if (strcmp(st, "authorizationStateWaitPhoneNumber") == 0 && !ctx->qr_sent) {
        ctx->qr_sent = 1;
        request_qr(ctx->client_id);
        return;
    }

    if (strcmp(st, "authorizationStateWaitOtherDeviceConfirmation") == 0) {
        show_qr_if_new(ctx,
            cJSON_GetStringValue(cJSON_GetObjectItem(state, "link")));
        return;
    }

    if (strcmp(st, "authorizationStateWaitPassword") == 0) {
        const char *hint = cJSON_GetStringValue(
            cJSON_GetObjectItem(state, "password_hint"));
        if (hint != NULL && hint[0] != '\0')
            printf("  (hint: %s)\n", hint);
        prompt_and_send_password(ctx);
        return;
    }

    if (strcmp(st, "authorizationStateReady") == 0) {
        ctx->ready = 1;
        if (!ctx->getme_sent) {
            cJSON *me;
            ctx->getme_sent = 1;
            me = cJSON_CreateObject();
            cJSON_AddStringToObject(me, "@type", "getMe");
            cJSON_AddStringToObject(me, "@extra", "tbox-auth");
            send_req(ctx->client_id, me);
        }
        return;
    }

    /* `close` results arrive NESTED here, not as a top-level
     * "authorizationStateClosed" object. This is the real exit. */
    if (strcmp(st, "authorizationStateClosed") == 0) {
        ctx->done = 1;
    }
}

/* user (getMe reply) -> print the account, then close. */
static void on_user(auth_ctx_t *ctx, const cJSON *upd)
{
    const char *first;
    const char *last;

    if (!extra_is(upd, "tbox-auth"))
        return;

    first = cJSON_GetStringValue(cJSON_GetObjectItem(upd, "first_name"));
    last = cJSON_GetStringValue(cJSON_GetObjectItem(upd, "last_name"));

    printf("  Logged in as %s %s\n",
           first != NULL ? first : "",
           last != NULL ? last : "");

    send_close(ctx);
}

/* error -> fatal unless it is a retryable wrong-2FA-password. */
static void on_error(auth_ctx_t *ctx, const cJSON *upd)
{
    const cJSON *code = cJSON_GetObjectItem(upd, "code");
    const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(upd, "message"));

    fprintf(stderr, "  TDLib error %d: %s\n",
            cJSON_IsNumber(code) ? (int)code->valuedouble : -1,
            msg != NULL ? msg : "");

    if (msg != NULL && strcmp(msg, "PASSWORD_HASH_INVALID") == 0) {
        /* TDLib re-sends authorizationStateWaitPassword: retry. */
        printf("  Wrong 2FA password - try again.\n");
        return;
    }

    ctx->fatal = 1;
}

static void on_update(auth_ctx_t *ctx, const cJSON *upd)
{
    const char *type = type_of(upd);

    if (strcmp(type, "updateAuthorizationState") == 0) {
        on_auth_state(ctx, upd);
    } else if (strcmp(type, "user") == 0) {
        on_user(ctx, upd);
    } else if (strcmp(type, "error") == 0) {
        on_error(ctx, upd);
    } else if (strcmp(type, "authorizationStateClosed") == 0) {
        ctx->done = 1;
    }
}

/* ---- the run loop ------------------------------------------------------ */

/* Pump td_receive until `authorizationStateClosed`, bounded by ticks.
 * Returns 1 if closed, 0 if the bound expired. */
static int drain_until_closed(auth_ctx_t *ctx, int max_ticks)
{
    int ticks;

    for (ticks = 0; ticks < max_ticks && !ctx->done; ticks++) {
        const char *raw = td_receive(RECV_TIMEOUT);
        cJSON *upd;

        if (raw == NULL)
            continue;
        upd = cJSON_Parse(raw);
        if (upd != NULL) {
            on_update(ctx, upd);
            cJSON_Delete(upd);
        }
    }

    return ctx->done;
}

int tbox_td_auth_run(const char *data_dir, volatile bool *should_stop)
{
    auth_ctx_t ctx;
    tbox_td_params_t params;
    cJSON *req;

    memset(&ctx, 0, sizeof ctx);
    memset(&params, 0, sizeof params);

    /* data dir: --data argument, else the per-OS default */
    if (data_dir != NULL && data_dir[0] != '\0')
        snprintf(params.data_dir, sizeof params.data_dir, "%s", data_dir);
    if (tbox_td_prepare_dir(&params) != 0) {
        fprintf(stderr, "tbox auth: cannot create data directory.\n");
        return TBOX_TD_ERROR;
    }

    /* credentials: TG_API_ID + TG_API_HASH from the environment only;
     * load_credentials prints its own reason on failure */
    if (tbox_td_load_credentials(&params) != 0)
        return TBOX_TD_ERROR;

    printf("  session dir: %s\n", params.data_dir);

    ctx.vt = tbox_term_enable_vt();

    /* Keep the terminal clean: TDLib's own log chatter off. The newer
     * td_json_client.h dropped the td_set_log_verbosity_level() export,
     * so use the synchronous option request instead. */
    (void)td_execute("{\"@type\":\"setLogVerbosityLevel\",\"new_level\":0}");

    ctx.client_id = td_create_client_id();

    req = tbox_td_build_parameters(&params);
    if (req == NULL)
        return TBOX_TD_ERROR;
    send_req(ctx.client_id, req);

    while (!ctx.done && !ctx.fatal) {
        const char *raw;
        cJSON *upd;

        /* cooperative cancellation: the signal thread flips should_stop */
        if (should_stop != NULL && *should_stop) {
            printf("  Cancelling login, closing TDLib...\n");
            if (!ctx.close_sent)
                send_close(&ctx);
            (void)drain_until_closed(&ctx, CLOSE_DRAIN_TICKS);
            return TBOX_TD_CANCELLED;
        }

        raw = td_receive(RECV_TIMEOUT);
        if (raw == NULL)
            continue; /* timeout: no update yet */

        upd = cJSON_Parse(raw);
        if (upd == NULL)
            continue;
        on_update(&ctx, upd);
        cJSON_Delete(upd);
    }

    if (ctx.fatal) {
        /* TDLib rule: every client must be closed before termination,
         * even after an error, or the session DB can end up dirty. */
        if (!ctx.close_sent)
            send_close(&ctx);
        (void)drain_until_closed(&ctx, CLOSE_DRAIN_TICKS);
        return TBOX_TD_ERROR;
    }

    return ctx.ready ? TBOX_TD_OK : TBOX_TD_ERROR;
}
