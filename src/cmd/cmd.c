/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Tarang Patel
 *
 * cmd.c — command implementations. Nothing here parses argv; the cli/
 * layer does that and guarantees arguments are present.
 */
#include "cmd/cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <conio.h>
#include <direct.h>
#include <windows.h>
#define TBOX_SLEEP_MS(ms) Sleep(ms)
#define TBOX_NOW_MS() ((unsigned long long)GetTickCount64())
#define TBOX_MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define TBOX_SLEEP_MS(ms) usleep((ms) * 1000)
#define TBOX_MKDIR(path) mkdir(path, 0755)
static unsigned long long tbox_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)ts.tv_nsec / 1000000ull;
}
#define TBOX_NOW_MS() tbox_now_ms()
#endif

/* Presentational names for the FSM enums (debug display only). */
static const char *STATE_NAMES[] = {
    "START", "WAIT_PARAMS", "WAIT_PHONE", "WAIT_QR", "WAIT_PASSWORD",
    "READY", "CLOSING", "CLOSED", "FAILED",
};
static const char *ACTION_NAMES[] = {
    "NONE", "REQUEST_PARAMS", "REQUEST_QR", "SHOW_QR", "ASK_PASSWORD",
    "AUTHORIZED", "CLOSED", "FATAL",
};

#include "cJSON.h"
#include "cli/cli.h"
#include "qr/qr.h"
#include "td/tbox_td.h"
#include "td/tbox_td_auth.h"
#include "td/tbox_td_client.h"

int tbox_cmd_version(void) {
    printf("tbox %s\n", TBOX_VERSION);
    return TBOX_EXIT_OK;
}

int tbox_cmd_auth_state(const char *td_state) {
    TBoxAuthTransition t = tbox_auth_next(TBOX_AUTH_START, td_state);
    if (t.next == TBOX_AUTH_FAILED) {
        fprintf(stderr, "error: unknown authorizationState '%s'\n", td_state);
        return TBOX_ERROR;
    }
    printf("%-42s -> state %-14s action %s\n",
           td_state, STATE_NAMES[t.next], ACTION_NAMES[t.action]);
    return TBOX_EXIT_OK;
}

int tbox_cmd_td_smoke(void) {
    tbox_td_init(1);
    char *out = tbox_td_execute("{\"@type\":\"getLogStream\"}");
    int rc = TBOX_EXIT_OK;
    if (out != NULL) {
        printf("getLogStream -> %s\n", out);
        free(out);
    } else {
        fprintf(stderr, "error: td_execute gave no reply\n");
        rc = TBOX_ERROR;
    }
    tbox_td_close();
    return rc;
}

/* ---- login --------------------------------------------------------- */

/* LoginCtx carries cross-thread flags; receive thread writes ints,
 * main thread reads them (single writer each, volatile for safety). */
typedef struct LoginCtx {
    TBoxTdClient *client;
    volatile int state;        /* last TBoxAuthState reached */
    volatile int done;         /* any terminal condition */
    volatile int exit_code;
    volatile int needs_password; /* receiver→main: prompt 2FA */
    volatile int params_sent;  /* setTdlibParameters sent (eager, once) */
    volatile int params_asks;  /* times tdlib asked for parameters */
    volatile unsigned long long params_reask_ms; /* when it re-asked (0 = never) */
    volatile int qr_sent;
    volatile int me_sent;
    volatile int cancelled;    /* user aborted at the password prompt */
    long api_id;               /* login config: passed IN, never read */
    const char *api_hash;
    char dbdir[600];
    char filesdir[600];
    char link[1024];           /* recv-thread-owned; printed there */
    char conn[64];             /* last updateConnectionState seen */
} LoginCtx;

static void login_send(TBoxTdClient *c, cJSON *req) {
    char *json = cJSON_PrintUnformatted(req);
    if (json != NULL) {
        tbox_td_client_send(c, json);
        cJSON_free(json);
    }
    cJSON_Delete(req);
}

/* Masked 2FA entry: '*' echo, backspace, Ctrl-C/Esc cancels. Ported from
 * the proven demo. Reads stdin on the MAIN thread only. Returns after
 * sending the password - or after setting cx->cancelled. */
static void login_prompt_password(LoginCtx *cx) {
    char pw[256];
    size_t len = 0;
    int cancelled = 0;

    printf("  Enter your 2FA password: ");
    fflush(stdout);
#ifdef _WIN32
    {
        int c = _getch();
        if (c < 0) {
            /* not an interactive console (stdin piped): _getch cannot
             * work, and looping on it would spin forever. Fall back. */
            if (fgets(pw, sizeof pw, stdin) == NULL) {
                cancelled = 1;
            }
            len = strcspn(pw, "\r\n");
        } else {
            while (c != 13 && c != 10) {
                if (c == 3 || c == 27) { /* Ctrl-C / Esc */
                    cancelled = 1;
                    break;
                }
                if (c == 224 || c == 0) { /* extended key prefix: eat code */
                    c = _getch();
                    continue;
                }
                if (c == 8 && len > 0) {
                    len--;
                    fputs("\b \b", stdout);
                    fflush(stdout);
                } else if (c >= 32 && len + 1 < sizeof pw) {
                    pw[len++] = (char)c;
                    fputc('*', stdout);
                    fflush(stdout);
                }
                c = _getch();
            }
        }
    }
#else
    if (fgets(pw, sizeof pw, stdin) == NULL) {
        cancelled = 1;
    }
    len = strcspn(pw, "\r\n");
#endif
    pw[len] = '\0';
    printf("\n");

    if (cancelled) {
        printf("  Cancelled - closing the session cleanly.\n");
        cx->cancelled = 1;
        cx->done = 1;
        cx->exit_code = TBOX_EXIT_OK;
        memset(pw, 0, sizeof pw);
        return;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "@type", "checkAuthenticationPassword");
    cJSON_AddStringToObject(req, "password", pw);
    cJSON_AddStringToObject(req, "@extra", "pwd");
    login_send(cx->client, req);
    memset(pw, 0, sizeof pw); /* no secrets left behind */
}

/* Sends setTdlibParameters (proven shape; db + files persist). Called
 * eagerly at startup - this tdlib build does not emit the opening
 * WaitTdlibParameters cue - and again if that state does show up. */
static void login_send_params(LoginCtx *cx) {
    cx->params_sent = 1;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "@type", "setTdlibParameters");
    cJSON_AddNumberToObject(req, "api_id", (double)cx->api_id);
    cJSON_AddStringToObject(req, "api_hash", cx->api_hash);
    cJSON_AddBoolToObject(req, "use_test_dc", 0);
    cJSON_AddStringToObject(req, "database_directory", cx->dbdir);
    cJSON_AddStringToObject(req, "files_directory", cx->filesdir);
    cJSON_AddBoolToObject(req, "use_file_database", 1);
    cJSON_AddBoolToObject(req, "use_message_database", 1);
    cJSON_AddBoolToObject(req, "use_secret_chats", 0);
    cJSON_AddStringToObject(req, "device_model", "tbox");
    cJSON_AddStringToObject(req, "system_language_code", "en");
    cJSON_AddStringToObject(req, "application_version", TBOX_VERSION);
    cJSON_AddStringToObject(req, "@extra", "init");
    login_send(cx->client, req);
}

static void login_update(void *user_data, const cJSON *update) {
    LoginCtx *cx = user_data;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(update, "@type");
    const char *t = cJSON_IsString(type) ? cJSON_GetStringValue(type) : "";

    /* Network visibility: silent terminals made a stuck login
     * indistinguishable from a slow one. */
    if (strcmp(t, "updateConnectionState") == 0) {
        const cJSON *cs = cJSON_GetObjectItemCaseSensitive(update, "state");
        const cJSON *cs_type = cJSON_GetObjectItemCaseSensitive(cs, "@type");
        const char *name = cJSON_IsString(cs_type) ? cJSON_GetStringValue(cs_type) : "";
        if (strcmp(name, cx->conn) != 0) {
            snprintf(cx->conn, sizeof cx->conn, "%s", name);
            printf("  connection: %s\n", name);
            fflush(stdout);
        }
        return;
    }

    if (strcmp(t, "updateAuthorizationState") != 0) {
        return; /* only auth updates drive login */
    }

    const cJSON *as = cJSON_GetObjectItemCaseSensitive(update, "authorization_state");
    const cJSON *as_type = cJSON_GetObjectItemCaseSensitive(as, "@type");
    const char *st = cJSON_IsString(as_type) ? cJSON_GetStringValue(as_type) : NULL;

    TBoxAuthTransition tr = tbox_auth_next((TBoxAuthState)cx->state, st);
    if (tr.next == TBOX_AUTH_FAILED) {
        fprintf(stderr, "  login: unexpected auth state '%s'\n", st ? st : "null");
        cx->done = 1;
        cx->exit_code = TBOX_ERROR;
        return;
    }
    cx->state = (int)tr.next;

    switch (tr.action) {
    case TBOX_AUTH_ACT_REQUEST_PARAMS:
        cx->params_asks++;
        if (!cx->params_sent) {
            /* asked before our eager send landed: answer the cue */
            printf("  tdlib asked for parameters - sending...\n");
            login_send_params(cx);
        } else if (cx->params_asks == 1) {
            /* normal in this tdlib build: it can ask once while our
             * (already sent) parameters are being processed. Re-sending
             * earns an "Unexpected setTdlibParameters" error, so wait
             * instead - the watchdog below catches a real rejection. */
            printf("  tdlib asked for parameters again (already sent) - waiting\n");
            cx->params_reask_ms = TBOX_NOW_MS();
        } else {
            fprintf(stderr,
                    "  tdlib keeps asking for parameters - see the TDLib error "
                    "above and check the api-id/api-hash pair\n"
                    "  (both from https://my.telegram.org/apps)\n");
            cx->done = 1;
            cx->exit_code = TBOX_ERROR;
        }
        break;
    case TBOX_AUTH_ACT_REQUEST_QR:
        if (!cx->qr_sent) {
            cx->qr_sent = 1;
            printf("  Requesting QR login (scan - no phone number is sent)...\n");
            cJSON *req = cJSON_CreateObject();
            cJSON_AddStringToObject(req, "@type", "requestQrCodeAuthentication");
            cJSON_AddItemToObject(req, "other_user_ids", cJSON_CreateArray());
            cJSON_AddStringToObject(req, "@extra", "qr");
            login_send(cx->client, req);
        }
        break;
    case TBOX_AUTH_ACT_SHOW_QR: {
        const char *link = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(as, "link"));
        if (link != NULL && strlen(link) < sizeof cx->link &&
            strcmp(link, cx->link) != 0) {
            snprintf(cx->link, sizeof cx->link, "%s", link);
            tbox_qr_render(link);
        }
        break;
    }
    case TBOX_AUTH_ACT_ASK_PASSWORD: {
        const char *hint = cJSON_GetStringValue(
            cJSON_GetObjectItemCaseSensitive(as, "password_hint"));
        if (hint != NULL && hint[0] != '\0') {
            printf("  (hint: %s)\n", hint);
        }
        cx->needs_password = 1;
        break;
    }
    case TBOX_AUTH_ACT_AUTHORIZED:
        if (!cx->me_sent) {
            cx->me_sent = 1;
            printf("  Authorized. Getting account name...\n");
            cJSON *req = cJSON_CreateObject();
            cJSON_AddStringToObject(req, "@type", "getMe");
            cJSON_AddStringToObject(req, "@extra", "me");
            login_send(cx->client, req);
        }
        break;
    case TBOX_AUTH_ACT_CLOSED:
        printf("  session closed.\n");
        cx->done = 1;
        break;
    case TBOX_AUTH_ACT_FATAL:
        cx->done = 1;
        cx->exit_code = TBOX_ERROR;
        break;
    case TBOX_AUTH_ACT_NONE:
    default:
        break;
    }
}

/* Replies (events with @extra): only errors and the getMe user matter. */
static int login_reply(LoginCtx *cx, const char *raw) {
    cJSON *root = cJSON_Parse(raw);
    if (root == NULL) {
        return 0;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "@type");
    const char *t = cJSON_IsString(type) ? cJSON_GetStringValue(type) : "";

    if (strcmp(t, "error") == 0) {
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        const char *m = cJSON_IsString(msg) ? cJSON_GetStringValue(msg) : "";
        fprintf(stderr, "  TDLib error: %s\n", m);
        if (strcmp(m, "PASSWORD_HASH_INVALID") != 0) {
            cx->done = 1;
            cx->exit_code = TBOX_ERROR;
        }
        /* wrong password: TDLib re-sends WaitPassword → we prompt again */
    } else if (strcmp(t, "user") == 0) {
        const cJSON *extra = cJSON_GetObjectItemCaseSensitive(root, "@extra");
        if (cJSON_IsString(extra) &&
            strcmp(cJSON_GetStringValue(extra), "me") == 0) {
            const char *first = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(root, "first_name"));
            const char *last = cJSON_GetStringValue(
                cJSON_GetObjectItemCaseSensitive(root, "last_name"));
            printf("  Logged in as %s %s\n", first ? first : "", last ? last : "");
            cJSON *close = cJSON_CreateObject();
            cJSON_AddStringToObject(close, "@type", "close");
            login_send(cx->client, close);
        }
    }
    /* other replies ("ok") need no handling */
    cJSON_Delete(root);
    return 0;
}

/* Strips trailing '/\' so "session\" and "session" give the same paths. */
static void login_dir(char *dst, size_t cap, const char *base, const char *leaf) {
    char clean[512];
    snprintf(clean, sizeof clean, "%s", base);
    size_t n = strlen(clean);
    while (n > 0 && (clean[n - 1] == '/' || clean[n - 1] == '\\')) {
        clean[--n] = '\0';
    }
    snprintf(dst, cap, "%s/%s", clean, leaf);
}

/* Creates every component of path. tdlib creates its database directory
 * but NOT its parent, so a fresh "session/tdlib" failed with
 * "Can't create directory" until the parent existed. Existence is fine. */
static void login_mkdirs(const char *path) {
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            (void)TBOX_MKDIR(tmp);
            *p = '/';
        }
    }
    (void)TBOX_MKDIR(tmp);
}

int tbox_cmd_login(const char *session_dir, const char *arg_api_id, const char *api_hash) {
    if (session_dir == NULL || session_dir[0] == '\0' ||
        arg_api_id == NULL || arg_api_id[0] == '\0' ||
        api_hash == NULL || api_hash[0] == '\0') {
        fprintf(stderr, "usage: tbox login <session-dir> <api-id> <api-hash>\n");
        return TBOX_UNKNOWN_COMMAND;
    }
    long api_id = strtol(arg_api_id, NULL, 10);
    if (api_id <= 0) {
        fprintf(stderr, "error: api-id '%s' is not a valid app id\n", arg_api_id);
        return TBOX_ERROR;
    }

    char dbdir[600], filesdir[600];
    login_dir(dbdir, sizeof dbdir, session_dir, "tdlib");
    login_dir(filesdir, sizeof filesdir, session_dir, "files");
    login_mkdirs(dbdir);
    login_mkdirs(filesdir);
    printf("tdlib session: %s (persists across runs)\n", dbdir);
    printf("(debug path: api credentials came from argv; the Python front "
           "will pass them programmatically)\n");

    TBoxTdClient *client = tbox_td_client_open(tbox_td_api(), 0);
    if (client == NULL) {
        fprintf(stderr, "error: could not open tdjson client\n");
        return TBOX_ERROR;
    }

    LoginCtx cx;
    memset(&cx, 0, sizeof cx);
    cx.client = client;
    cx.state = (int)TBOX_AUTH_START;
    cx.api_id = api_id;
    cx.api_hash = api_hash;
    snprintf(cx.dbdir, sizeof cx.dbdir, "%s", dbdir);
    snprintf(cx.filesdir, sizeof cx.filesdir, "%s", filesdir);
    tbox_td_client_on_update(client, login_update, &cx);

    if (tbox_td_client_start(client) != 0) {
        fprintf(stderr, "error: could not start the tdjson receive thread\n");
        tbox_td_client_close(client);
        return TBOX_ERROR;
    }

    /* Eager parameters (the proven demo order): video-verified that this
     * tdlib build does not always emit the opening cue, so waiting for it
     * can idle forever. The FSM row stays as the rejection detector. */
    printf("  sending tdlib parameters...\n");
    login_send_params(&cx);

#ifdef _WIN32
    tbox_qr_console_setup(); /* best effort: ## grid fallback otherwise */
#endif

    /* No eager requests: tdlib cues each step (params first) through
     * updateAuthorizationState, the handler answers on the table's word. */

    unsigned long long started = TBOX_NOW_MS();
    unsigned long long last_beat = started;

    while (!cx.done) {
        if (cx.needs_password) {
            cx.needs_password = 0;
            login_prompt_password(&cx);
            last_beat = TBOX_NOW_MS();
        }
        char *reply = tbox_td_client_take_reply(client);
        if (reply != NULL) {
            login_reply(&cx, reply);
            free(reply);
        }
        unsigned long long now = TBOX_NOW_MS();

        /* watchdog: a re-ask with no follow-up auth state means tdlib is
         * sitting on rejected parameters (it replies with a state, not
         * always an error object) */
        if (cx.params_reask_ms != 0 &&
            (int)cx.state == (int)TBOX_AUTH_WAIT_PARAMS &&
            now - cx.params_reask_ms > 20000ull) {
            fprintf(stderr,
                    "  no auth progress %llus after the parameter re-ask - "
                    "parameters were likely rejected\n"
                    "  check the api-id/api-hash pair (see any TDLib error above)\n",
                    (now - cx.params_reask_ms) / 1000ull);
            cx.done = 1;
            cx.exit_code = TBOX_ERROR;
        }

        if (now - last_beat >= 30000ull) {
            last_beat = now;
            unsigned long long secs = (now - started) / 1000ull;
            const char *st = ((int)cx.state >= 0 && (int)cx.state <= TBOX_AUTH_FAILED)
                                 ? STATE_NAMES[cx.state] : "?";
            if ((int)cx.state == (int)TBOX_AUTH_WAIT_QR) {
                printf("  waiting %llus for the scan (the QR rotates by itself; "
                       "Ctrl-C aborts)\n", secs);
            } else {
                printf("  waiting %llus (state %s)\n", secs, st);
            }
            fflush(stdout);
        }
        TBOX_SLEEP_MS(50);
    }
    tbox_td_client_close(client);
    return cx.exit_code;
}
