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

    TBoxSession *s = tbox_session_start(tbox_td_api(), session_dir, api_id, api_hash,
                                        NULL);
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
            goto done;
        case TBOX_SESSION_EV_ERROR:
            event_line("error", "message", ev.code, ev.data);
            /* a rejected 2FA password is recoverable: tdlib asks again,
             * which reaches the front as a fresh password_needed */
            if (strcmp(ev.data, "PASSWORD_HASH_INVALID") != 0) {
                rc = TBOX_ERROR;
                goto done;
            }
            break;
        }
    }

done:
    tbox_session_close(s);
    return rc;
}