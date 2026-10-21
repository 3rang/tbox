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

#include "cJSON.h"
#include "cli/cli.h"
#include "service/tbox_session.h"
#include "td/tbox_td.h"
#include "td/tbox_td_auth.h"

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
           td_state, tbox_auth_state_name(t.next), tbox_auth_action_name(t.action));
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

/* ---- login ---------------------------------------------------------- */

/* One machine-readable line per event: this command has no user interface
 * on purpose. A front end (Python TUI/GUI, a pipe, a script) reads these
 * and decides what to draw; the QR link is handed over, never painted.
 * Spec: {"event":<name>, "state"|"link"|"hint"|"user"|"message":<value>,
 *        "code":<n> only for errors} */
static void event_line(const char *event, const char *key, int code,
                       const char *value) {
    cJSON *o = cJSON_CreateObject();
    if (o == NULL) {
        return;
    }
    cJSON_AddStringToObject(o, "event", event);
    if (key != NULL) {
        cJSON_AddStringToObject(o, key, value != NULL ? value : "");
    }
    if (code != 0) {
        cJSON_AddNumberToObject(o, "code", code);
    }
    char *line = cJSON_PrintUnformatted(o);
    if (line != NULL) {
        printf("%s\n", line);
        fflush(stdout);
        cJSON_free(line);
    }
    cJSON_Delete(o);
}

int tbox_cmd_login(void) {
    /* Credentials come from one JSON line on stdin (P1: never argv). The
     * line is not echoed anywhere and the api_hash/db_key are used once. */
    char creds[2048];
    if (fgets(creds, sizeof creds, stdin) == NULL) {
        fprintf(stderr, "error: expected credentials JSON on stdin\n");
        return TBOX_UNKNOWN_COMMAND;
    }
    cJSON *cj = cJSON_Parse(creds);
    if (cj == NULL) {
        fprintf(stderr, "error: credentials are not valid JSON\n");
        return TBOX_UNKNOWN_COMMAND;
    }
    const char *api_id_s = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(cj, "api_id"));
    const char *api_hash = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(cj, "api_hash"));
    const char *db_key = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(cj, "db_key"));
    const char *session_dir = cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(cj, "session_dir"));

    if (session_dir == NULL || session_dir[0] == '\0' ||
        api_id_s == NULL || api_hash == NULL || api_hash[0] == '\0') {
        fprintf(stderr,
                "error: credentials need session_dir, api_id and api_hash\n");
        cJSON_Delete(cj);
        return TBOX_UNKNOWN_COMMAND;
    }
    long api_id = strtol(api_id_s, NULL, 10);
    if (api_id <= 0) {
        fprintf(stderr, "error: api_id is not a valid app id\n");
        cJSON_Delete(cj);
        return TBOX_ERROR;
    }

    TBoxSession *s = tbox_session_start(tbox_td_api(), session_dir, api_id,
                                        api_hash, db_key);
    cJSON_Delete(cj); /* the secrets do not outlive this call */
    if (s == NULL) {
        fprintf(stderr, "error: could not start the tdlib session\n");
        return TBOX_ERROR;
    }

    int rc = TBOX_EXIT_OK;
    for (;;) {
        TBoxSessionEventOut ev;
        if (!tbox_session_take_event(s, &ev, 200)) {
            continue; /* nothing yet; the front owns any give-up timeout */
        }
        switch (ev.type) {
        case TBOX_SESSION_EV_AUTH_STATE:
            event_line("auth_state", "state", 0, ev.data);
            break;
        case TBOX_SESSION_EV_CONNECTION:
            event_line("connection", "state", 0, ev.data);
            break;
        case TBOX_SESSION_EV_QR_LINK:
            event_line("qr_link", "link", 0, ev.data);
            break;
        case TBOX_SESSION_EV_PASSWORD_NEEDED: {
            event_line("password_needed", "hint", 0, ev.data);
            /* unmasked line on purpose: hiding what is typed is a front
             * concern, not an engine one. */
            char pw[256];
            if (fgets(pw, sizeof pw, stdin) == NULL) {
                event_line("cancelled", NULL, 0, NULL);
                free(ev.data);
                goto done;
            }
            pw[strcspn(pw, "\r\n")] = '\0';
            tbox_session_submit_password(s, pw);
            memset(pw, 0, sizeof pw);
            break;
        }
        case TBOX_SESSION_EV_READY:
            event_line("ready", "user", 0, ev.data);
            tbox_session_request_close(s);
            break;
        case TBOX_SESSION_EV_CLOSED:
            event_line("closed", NULL, 0, NULL);
            free(ev.data);
            goto done;
        case TBOX_SESSION_EV_ERROR:
            event_line("error", "message", ev.code, ev.data);
            /* a rejected 2FA password is recoverable: tdlib asks again,
             * which reaches the front as a fresh password_needed */
            if (ev.data == NULL ||
                strcmp(ev.data, "PASSWORD_HASH_INVALID") != 0) {
                free(ev.data);
                rc = TBOX_ERROR;
                goto done;
            }
            break;
        default:
            break; /* transfer/sync events belong to the engine, not this command */
        }
        free(ev.data);
    }

done:
    tbox_session_close(s);
    return rc;
}